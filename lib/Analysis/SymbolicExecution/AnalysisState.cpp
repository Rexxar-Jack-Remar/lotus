//===----------------------------------------------------------------------===//
//
// AnalysisState implementation.
// Handles state transfer for instructions, memory object management,
// and expression evaluation for symbolic execution.
//
//===----------------------------------------------------------------------===//

#include "Analysis/SymbolicExecution/AnalysisState.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#include "Analysis/SymbolicExecution/AnalysisDriver.h"
#include "Analysis/SymbolicExecution/DomTreePass.h"
#include "Analysis/SymbolicExecution/InstResolver.h"
#include "Analysis/SymbolicExecution/MemoryAPI.h"
#include "Analysis/SymbolicExecution/PropertyAllocator.h"
#include "Analysis/SymbolicExecution/PropertyInteger.h"
#include "Analysis/SymbolicExecution/PropertySym.h"
#include "Analysis/SymbolicExecution/TaintModel.h"

#include <functional>
#include <numeric>

#define DEBUG_TYPE "Symex"

cl::opt<bool> AnalyzeNonArrayAlloca(
    "symex-analyze-nonarray-alloca",
    cl::desc("Treat nonarray alloca also as memory allocation sites"),
    cl::init(false));

cl::opt<std::string> SymexDebugFunName("symex-debug-fun", cl::init(""));

static std::mutex AliasQueryMtx;

using namespace SymbolicExecution;

#define REGISTE_LIMIT(FieldName, OptName, Val)                                 \
  unsigned AnalysisLimit::FieldName;                                           \
  static cl::opt<unsigned, true> XX_##FieldName(                               \
      OptName, cl::location(AnalysisLimit::FieldName), cl::init(Val));

REGISTE_LIMIT(INST_QUERY_LIMIT_V, "symex-inst-query-limit", 5)
REGISTE_LIMIT(FUNC_QUERY_LIMIT_V, "symex-func-query-limit", 200)
REGISTE_LIMIT(VALUE_SET_LIMIT_V, "symex-value-set-limit", 20)
REGISTE_LIMIT(SYMBOLIC_VAL_SET_LIMIT_V, "symex-sym-value-set-limit", 20)
REGISTE_LIMIT(POINTS_SET_LIMIT_V, "symex-pts-set-limit", 20)
REGISTE_LIMIT(TAINT_VAL_SET_LIMIT_V, "symex-taint-set-limit", 30000)
REGISTE_LIMIT(FUNC_INLINE_LIMIT_V, "symex-func-inline-limit", 10)
REGISTE_LIMIT(CONSTRAINT_SIZE_LIMIT_V, "symex-constraint-sz-limit", 1000)
REGISTE_LIMIT(MAX_FUNC_SOLVER_LIMIT_V, "symex-max-func-solver-limit", 1024);

Type *AnalysisState::NON_PTR_TY = nullptr;

Type *AnalysisState::INT8_TY = nullptr;

namespace {

bool isFreeLikeFunction(const Function *callee) {
  if (!callee) {
    return false;
  }

  StringRef name = callee->getName();
  return name == "free" || name == "_ZdlPv" || name == "_ZdaPv";
}

} // namespace

AnalysisState::AnalysisState(SymexBugType BugTy, GuardedValueFlowGraph *Graph,
                             Function *Func)
    : BugTy(BugTy), Graph(Graph), F(Func),
      TaintSpec(gvfg_utility::getTaintSpec()), Solver(new PathCondSolver()) {
  // Seed the entry state from the GVFG interface. Pointer-like formals start
  // with a symbolic heap object so later loads/stores have something to talk
  // about, while every argument also gets a symbolic register value because
  // callers may constrain scalars even when no precise definition exists here.
  for (auto Iter = Graph->arg_begin(), EIter = Graph->arg_end(); Iter != EIter;
       ++Iter) {
    // common arg or pseduo arg node
    auto *ArgNode = *Iter;
    if (ArgNode->getType()->isPointerTy()) {
      createMemoryObject(ArgNode, PTItem::MK_SYMBOLIC);
    }
    // ArgNode takes unknown values
    initSymbol(ArgNode);
  }

  taintInit(Func);
}

GuardedValueFlowNode *AnalysisState::getNode(Value *V) const {
  assert(V && "getNode() expects non-null Value");
  // Most callers already strip pointer casts, but doing it here makes the
  // interface more robust.
  V = V->stripPointerCasts();

  auto *N = Graph->findNode(V);
  if (!N) {
    // The GVFG is immutable once constructed. If we can't find a node, treat
    // this as a construction/usage bug rather than mutating the graph.
    // Keep the assertion to avoid silently producing unsound results.
    llvm_unreachable("Unable to find GVFG node for LLVM Value");
  }
  return N;
}

/// Create a new abstract memory object at Ptr.
/// Kind distinguishes between concrete (known size), symbolic (unknown size but
/// valid), and placeholder (unknown validity).
void AnalysisState::createMemoryObject(const ProgramValuePtr &Ptr,
                                       PTItem::MemObjKind Kind,
                                       const PropertyValuePtr &Sz,
                                       const Condition &Cond) {
  // The points-to map is the heap model for this executor. We attach a guarded
  // PTItem instead of mutating a separate heap store so one program value can
  // simultaneously name several abstract objects under different path facts.
  const auto &Pt = PTItem(Ptr, Kind, 0, Sz);
  PointsTo[Ptr].addValue(Pt, Cond);
}

/// Compute offset expression for GEP instruction.
PropertyValuePtr AnalysisState::computeOffsets(GEPOperator *GEP) {
  assert(!GEP->getType()->isVectorTy());
  PropertySymExpr OffsetExpr;
  int64_t ConstantOffset = 0;

  Type *current_type = GEP->getSourceElementType();
  for (Value *index_operand : GEP->indices()) {
    if (auto *st = dyn_cast<StructType>(current_type)) {
      const ConstantInt *ci = cast<ConstantInt>(index_operand);
      uint64_t Addend =
          gvfg_utility::getElementOffset(st, (unsigned)ci->getZExtValue());
      ConstantOffset += static_cast<int64_t>(Addend);
      current_type =
          st->getElementType(static_cast<unsigned>(ci->getZExtValue()));
      continue;
    }

    Type *element_type = nullptr;
    if (auto *array_ty = dyn_cast<ArrayType>(current_type))
      element_type = array_ty->getElementType();
    else if (auto *vector_ty = dyn_cast<VectorType>(current_type))
      element_type = vector_ty->getElementType();
    else if (auto *ptr_ty = dyn_cast<PointerType>(current_type))
      element_type = ptr_ty->getPointerElementType();
    else
      assert(false && "invalid GEP type");

    uint64_t ElementSize = gvfg_utility::getTypeStoreSize(element_type);
    auto *IndexVal = getNode(index_operand);
    auto *CI = dyn_cast<ConstantInt>(IndexVal->getLLVMValue());
    if (CI) {
      ConstantOffset += CI->getSExtValue() * (int64_t)ElementSize;
    } else {
      OffsetExpr.addTerm(Var(IndexVal), ElementSize);
    }
    current_type = element_type;
  }

  if (OffsetExpr.isConstant()) {
    ConstantOffset += OffsetExpr.getAsConstant().getAsBoundInt();
    return GetProperty<PropertyInteger>(ConstantOffset);
  } else {
    OffsetExpr.addConstTerm(ConstantOffset);
    return GetProperty<PropertySymExpr>(OffsetExpr);
  }
}

// Insert V to Regs[V] as a last resort
void AnalysisState::initSymbol(const ProgramValuePtr &V) {
  if (V.isVacuous()) {
    return;
  }

  if (Regs.count(V) && !Regs.at(V).empty()) {
    return;
  }

  PropertyValuePtr PVP(V);
  if (!PVP) {
    // PropertyValuePtr construction failed, skip this value
    llvm::errs()
        << "Warning: Failed to create PropertyValuePtr in initSymbol\n";
    return;
  }

  Regs[V].addValue(PVP);
}

/// Main transfer function dispatching based on instruction type.
void AnalysisState::transfer(Instruction *Inst, AnalysisDriver &Driver) {
  Function *CurFun = Inst->getParent()->getParent();
  // The return insts should be unified.
  assert(!Driver.hasSummary(CurFun));
  // Normalize non-instruction operands up front. Most transfer helpers assume
  // `Regs` already has an entry for constants, arguments, and function values,
  // and `initSymbol` gives them an explicit unknown when no stronger fact is
  // available yet.
  // example: branch inst --- create val for cond
  // gep p 0 1 2  -- create val for const
  for (unsigned Idx = 0; Idx < Inst->getNumOperands(); ++Idx) {
    // The operand may even be a Function, e.g.,
    // %cond = phi i32 (i8*, i8*)* [ @strcmp, %cond.true ], [ %c,
    // %cond.false ]
    auto *Opnd = Inst->getOperand(Idx);
    if (!isa<Instruction>(Opnd)) {
      initSymbol(getNode(Opnd));
      initSymbol(getNode(Opnd->stripPointerCasts()));
    }
  }

  auto OpC = Inst->getOpcode();
  switch (OpC) {
  // Modelled binary Inst
  case llvm::Instruction::UDiv:
  case llvm::Instruction::SDiv:
  case llvm::Instruction::Mul:
  case llvm::Instruction::Add:
  case llvm::Instruction::Sub: {
    processBinaryInst(Inst);
    break;
  }
  // Unmodelled binary Inst ==> treat as unknown
  case llvm::Instruction::FMul:
  case llvm::Instruction::FAdd:
  case llvm::Instruction::FSub:
  case llvm::Instruction::And:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
  case llvm::Instruction::LShr:
  case llvm::Instruction::AShr:
  case llvm::Instruction::FDiv:
  case llvm::Instruction::SRem:
  case llvm::Instruction::FRem:
  case llvm::Instruction::URem:
  // Other unmodelled insts
  case llvm::Instruction::ExtractElement:
  case llvm::Instruction::InsertElement:
  case llvm::Instruction::FCmp:
  case llvm::Instruction::Select: {
    break;
  }
  case llvm::Instruction::Shl: {
    // %tmp22 = shl i64 %tmp19, 3 <==> %tmp22 = %tmp19 * 8
    if (isa<ConstantInt>(Inst->getOperand(1)) &&
        cast<ConstantInt>(Inst->getOperand(1))->getZExtValue() < 64) {
      auto Cst = PropertyInteger(
          (1ull) << cast<ConstantInt>(Inst->getOperand(1))->getZExtValue());
      auto Res = Regs.at(getNode(Inst->getOperand(0))) * Cst;
      if (!Res.empty()) {
        Regs[getNode(Inst)] = std::move(Res);
      }
    }
    break;
  }
  case llvm::Instruction::ICmp: {
    processICmpInst(Inst);
    break;
  }
  case Instruction::Alloca: {
    processAlloca(Inst);
    break;
  }
  // Conversion Inst
  case llvm::Instruction::AddrSpaceCast:
  case llvm::Instruction::IntToPtr:
  case llvm::Instruction::PtrToInt:
  case llvm::Instruction::BitCast:
  case llvm::Instruction::ZExt:
  case llvm::Instruction::SExt:
  case llvm::Instruction::Trunc:
  case llvm::Instruction::FPTrunc:
  case llvm::Instruction::FPExt:
  case llvm::Instruction::SIToFP:
  case llvm::Instruction::FPToSI:
  case llvm::Instruction::UIToFP:
  case llvm::Instruction::FPToUI: {
    assignVal(getNode(Inst), getNode(Inst->getOperand(0)->stripPointerCasts()));
    break;
  }
  case Instruction::GetElementPtr: {
    processGEP(Inst);
    break;
  }
  case Instruction::Load: {
    processLoad(Inst);
    break;
  }
  case Instruction::Store: {
    processStore(Inst);
    break;
  }
  case Instruction::PHI: {
    processPhiInst(Inst);
    break;
  }
  case llvm::Instruction::Call: {
    Function *Callee = gvfg_utility::getCallee(Inst);
    auto *CallI = cast<CallInst>(Inst);

    auto AllocSizeArgsIdx = gvfg_utility::getMemSpec()->getHeapAllocSize(CallI);
    // The called function is a primitive memory allocation function (could
    // be defined or a lib).
    if (!AllocSizeArgsIdx.empty()) {
      std::vector<unsigned> AllocSizeArgs;
      AllocSizeArgs.reserve(AllocSizeArgsIdx.size());
      for (int I : AllocSizeArgsIdx) {
        if (I >= 0) {
          AllocSizeArgs.push_back(static_cast<unsigned>(I));
        }
      }
      processMemAlloc(CallI, AllocSizeArgs);
    } else {
      // Calls split into three cases: unresolved/declaration calls go through
      // library modeling, summarized definitions are instantiated on demand,
      // and deep recursion falls back to the unknown-library model to avoid
      // unbounded inlining.
      // Callee == nullptr, i.e., unresolved function pointers.
      // or Callee->isDeclaration()
      if (!gvfg_utility::isDefiniteCall(Inst)) {
        processLibraryCall(CallI);
      } else if (Driver.hasSummary(
                     Callee)) { // skip recursive calls, f call f itself.
        unsigned FunDepth = gvfg_utility::getFunctionDepth(Callee);
        if (FunDepth > AnalysisLimit::FUNC_INLINE_LIMIT_V) {
          processAsUnknownLib(CallI);
        } else {
          processCall(CallI, Callee, Driver.getSummary(Callee));
        }
      }
    }

    break;
  }
  case llvm::Instruction::Ret: {
    this->RetI = cast<ReturnInst>(Inst);
    break;
  }
  }

  if (!Inst->getType()->isVoidTy()) {
    // Only add a dummy symbol in case no value is produced.
    initSymbol(getNode(Inst));
  }

  taintTransfer(Inst);
  buildQuery(Inst);
}

void AnalysisState::processBinaryInst(Instruction *Inst) {
  auto OpC = PropertyValue::fromLLVMOp(Inst->getOpcode());
  auto Res = Regs.at(getNode(Inst->getOperand(0)))
                 .binOp(Regs.at(getNode(Inst->getOperand(1))), OpC);
  if (!Res.empty()) {
    Regs[getNode(Inst)] = std::move(Res);
  }
}

void AnalysisState::processICmpInst(Instruction *Inst) {
  auto *CmpI = cast<CmpInst>(Inst);
  unsigned Pred = CmpI->getPredicate();
  auto Res = Regs.at(getNode(Inst->getOperand(0)))
                 .cmp(Regs.at(getNode(Inst->getOperand(1))), Pred);

  auto *Dst = getNode(Inst);
  Regs[Dst].addValue(GetProperty<PropertyInteger>(0), Res[0]);
  Regs[Dst].addValue(GetProperty<PropertyInteger>(1), Res[1]);
}

void AnalysisState::processAlloca(Instruction *Inst) {
  auto *AllocI = cast<AllocaInst>(Inst);
  Type *AllocatedTy = AllocI->getAllocatedType();
  auto *NumElements = getNode(AllocI->getArraySize());
  auto *Dst = getNode(Inst);

  uint64_t TySize = gvfg_utility::getTypeStoreSizeInBits(AllocatedTy);
  // For empty struct such as struct T {}
  if (TySize == 0) {
    TySize = 8;
  }

  ProgramValuePtr _dstV(Dst);
  ProgramValuePtr _numV(NumElements);
  ProgramValuePtr NumV(NumElements);
  if (NumV.isVacuous()) {
    // A vacuous size means the builder could not recover a meaningful array
    // bound. Keep the location alive with a placeholder object so downstream
    // checks still see a dereferenceable abstract region.
    createMemoryObject(Dst, PTItem::MK_PLACEHOLDER);
    // Avoid crashing and return a conservative object.
    return;
  }

  if (!NumV.isConstant() && !Regs.count(NumElements)) {
    initSymbol(NumElements);
    if (!Regs.count(NumElements)) {
      createMemoryObject(Dst, PTItem::MK_PLACEHOLDER);
      return;
    }
  }
  if (NumV.isConstant()) {
    BigInteger Sz = NumV.getAsConstant() * TySize;
    PropertyValuePtr AllocSize = GetProperty<PropertyInteger>(std::move(Sz));
    createMemoryObject(Dst, PTItem::MK_CONCRETE, AllocSize);
  } else {
    // Non-constant stack extents stay path-sensitive. Each guarded symbolic
    // size becomes its own memory object so later BOF checks can reason about
    // feasible sizes rather than collapsing them to one unknown blob.
    auto SizeVals = Regs.at(NumElements) * PropertyInteger(TySize);
    for (const auto &SizeCond : SizeVals) {
      createMemoryObject(Dst, PTItem::MK_CONCRETE, SizeCond.first,
                         SizeCond.second);
    }
  }
  assert(!PointsTo.at(Dst).empty());
}

void AnalysisState::setPts(const ProgramValuePtr &Dst, const PtsSet &Pts) {
  PointsTo[Dst] = Pts;
}

void AnalysisState::assignVal(const ProgramValuePtr &Dst,
                              const ProgramValuePtr &Src,
                              const Condition &Cond) {
  // Ensure both endpoints have symbols so lookups do not throw.
  if (!Regs.count(Src)) {
    initSymbol(Src);
  }
  initSymbol(Dst);

  if (Regs.count(Src)) {
    Regs[Dst].addValues(Regs.at(Src), Cond);
  }

  // Pointer-typed facts travel alongside scalar facts. This keeps the value
  // domain and the points-to domain synchronized when a move/cast copies a
  // pointer without touching memory.
  if (PointsTo.count(Src)) {
    PointsTo[Dst].addValues(PointsTo.at(Src), Cond);
  }
}

void AnalysisState::assignVals(const ProgramValuePtr &Dst,
                               const GuardedProgramValSet &SrcVals) {
  assert(!Regs.count(Dst));
  assert(!PointsTo.count(Dst));
  for (const auto &P : SrcVals) {
    const auto &V = P.first;
    const auto &Cond = P.second;
    initSymbol(V);

    Regs[Dst].addValues(Regs.at(V), Cond);
    if (PointsTo.count(V)) {
      PointsTo[Dst].addValues(PointsTo.at(V), Cond);
    }
  }
}

void AnalysisState::assignPtr(const ProgramValuePtr &Dst,
                              const ProgramValuePtr &Src,
                              const GuardedSymbolicValSet &Offset,
                              bool StrongUpdate, const Condition &AssignCond) {
  if (!PointsTo.count(Src)) {
    return;
  }

  auto &DstPts = PointsTo[Dst];
  if (StrongUpdate) {
    DstPts.clear();
  }

  if (DstPts.isFull()) {
    return;
  }

  const auto &SrcPts = PointsTo.at(Src);
  // Pointer arithmetic is modeled by shifting every reachable abstract object
  // by every feasible offset. The destination is optionally strong-updated
  // because GEP results replace an SSA value, not because the underlying heap
  // object changed.
  SrcPts.forEach2(
      Offset,
      [&](const PTItem &Target, const PropertyValuePtr &OffVal,
          const Condition &SrcCond) {
        DstPts.addValue(Target.offsetBy(OffVal), SrcCond && AssignCond);
      },
      [&] { return DstPts.isFull(); });
}

void AnalysisState::processGEP(Instruction *Inst) {
  if (!Inst->getType()->isPointerTy()) { // vector type
    return;
  }
  auto *GEP = cast<GEPOperator>(Inst);
  PropertyValuePtr Off = computeOffsets(GEP);
  // FIXME
  auto OffsetVals = evalExpr(Off, Regs);
  Condition IndexDataDeps;
  if (IsaProperty<PropertySymExpr>(Off)) {
    const auto *SymE = CastProperty<PropertySymExpr>(Off);
    for (auto Iter = SymE->begin(), EIter = SymE->end(); Iter != EIter;
         ++Iter) {
      ProgramValuePtr X = Iter->first.getValue();
      if (X.isa<GuardedValueFlowNodeValue>()) {
        auto *N = X.getAs<GuardedValueFlowNodeValue>()->getNode();
        // Condition CurDeps(Solver->getDataDeps(N), Solver.get());
        // IndexDataDeps = IndexDataDeps && CurDeps && getCallSiteOutDeps();

        IndexDataDeps = IndexDataDeps && getDataDepsCond(N);
      }
    }
  }
  OffsetVals = OffsetVals && IndexDataDeps;

  auto *SrcPtr = getNode(gvfg_utility::getPointerOperand(GEP));
  initPointsToTarget(SrcPtr, Inst);

  // `PropertySymExpr` offsets are computed in bytes, mirroring LLVM GEP
  // semantics, while PTItem offsets are tracked in bits. Keep both views: the
  // points-to set feeds memory-region reasoning, and the register value keeps
  // the affine pointer expression used by later arithmetic and summary code.
  auto OffsetInBits = OffsetVals * PropertyInteger(8);

  auto *Dst = getNode(Inst);

  assert(!PointsTo.at(SrcPtr).empty());
  assignPtr(Dst, SrcPtr, OffsetInBits, true);

  if (PointsTo.at(Dst).empty()) {
    assignPtr(Dst, SrcPtr, Off * PropertyInteger(8), true);
  }

  // Store
  Regs[Dst] = Regs.at(SrcPtr) + OffsetVals;

  assert(!PointsTo.at(Dst).empty());
}

static bool isGloablLLVMVal(const ProgramValuePtr &V) {
  return V.isa<GuardedValueFlowNodeValue>() &&
         isa<GlobalVariable>(V.getLLVMVal());
}

void AnalysisState::initPointsToTarget(const ProgramValuePtr &Ptr,
                                       Instruction *Pos) {
  (void)Pos;
  if (PointsTo.count(Ptr) && !PointsTo.at(Ptr).empty()) {
    return;
  }

  // Loads, stores, and unknown calls require every dereferenceable pointer to
  // name at least one abstract region. Globals and pseudo-arguments are treated
  // as valid but size-unknown objects, while everything else falls back to a
  // placeholder region so the executor stays conservative instead of failing.
  if (isGloablLLVMVal(Ptr) || isPseudoArgVal(Ptr)) {
    createMemoryObject(Ptr, PTItem::MK_SYMBOLIC);
  } else {
    createMemoryObject(Ptr, PTItem::MK_PLACEHOLDER);
  }

  initSymbol(Ptr);
  assert(!PointsTo.at(Ptr).empty());
}

void AnalysisState::processLoad(Instruction *Inst) {
  auto *LoadI = cast<LoadInst>(Inst);
  auto *LdPtr = getNode(gvfg_utility::getPointerOperand(LoadI));

  initPointsToTarget(LdPtr, Inst);

  auto *LdInstNode = getNode(Inst);
  if (LdInstNode->getNumChildren() >= 1) {
    if (auto *LdMemNode =
            dyn_cast<GuardedValueFlowNode>(LdInstNode->getChild(0))) {
      // The GVFG memory child identifies which reaching definitions can feed
      // this load. `processLoadPtr` then materializes the guarded values from
      // that region into the SSA destination.
      auto *Dst = getNode(Inst);
      processLoadPtr(LdPtr, LdMemNode, Dst, Inst);
    }
  }
}

void AnalysisState::processLoadPtr(const ProgramValuePtr &Ptr,
                                   const GuardedValueFlowNode *LdMemNode,
                                   const ProgramValuePtr &Dst,
                                   Instruction *Pos) {
  if (!Ptr.isNull()) {
    initSymbol(Ptr);
  }

  auto Incomings = gvfg_utility::getIncomingValuesForLoad(LdMemNode);

  // Loads do not read a concrete heap cell here. Instead they merge the GVFG
  // producers that reach the memory node, guarded by the region condition that
  // says when that producer is the active store for this access.
  for (auto &Item : Incomings) {
    Condition Cond = getRegionCond(Item.second);

    initSymbol(Item.first);
    assignVal(Dst, Item.first, Cond);

    if (!Ptr.isNull()) {
      propagateTaintPointer(Ptr, Item.first, Pos, Cond);
    }

    if (hasSymbolicVals(Dst)) {
      propagateTaint(Item.first, Dst, Pos, Cond, false);
    }
  }
}

void AnalysisState::collectEscapeObjs() {
  // NOTE: Older versions used builder helper APIs to compute escaping
  // allocation sites. Those APIs are no longer available, and the GVFG stays
  // immutable after construction. For now we keep escape info empty.
  //
  // This is a precision loss but keeps the analysis well-defined and the build
  // working; downstream inlining logic tolerates an empty escape set.
  // FIXME: this seems to be a severe limitatition...
  return;
}

void AnalysisState::processReturn() {
  auto *RetNode = Graph->getCommonReturn();
  if (RetNode) {
    // Summaries export through GVFG return nodes rather than raw LLVM return
    // instructions. This keeps ordinary returns and pseudo returns in one
    // format so callers can instantiate them uniformly.
    if (RetNode->getNumChildren() >= 1) {
      auto *RetValNode = cast<GuardedValueFlowNode>(RetNode->getChild(0));
      initSymbol(RetValNode);
      assignVal(RetNode, RetValNode);
      propagateTaint(RetValNode, RetNode, nullptr, Condition(), false);
    } else {
      initSymbol(RetNode);
    }
    OutSymbolicValMap[RetNode].addValues(Regs.at(RetNode));
  }

  for (auto Iter = Graph->pseudo_return_begin(),
            EIter = Graph->pseudo_return_end();
       Iter != EIter; ++Iter) {
    auto *RetNode = cast<GuardedValueFlowReturnNode>(*Iter);
    Value *BasePtr = RetNode->getAccessPath().getBase();
    ProgramValuePtr LdPtr = BasePtr ? getNode(BasePtr) : ProgramValuePtr();

    for (size_t I = 0; I < RetNode->getNumChildren(); ++I) {
      auto *Ch = RetNode->getChild(I);
      if (auto *LdMemNode = dyn_cast<GuardedValueFlowNode>(Ch)) {
        processLoadPtr(LdPtr, LdMemNode, RetNode, RetI);
      }
    }

    // In case no value is fetched into RetNode
    initSymbol(RetNode);
    OutSymbolicValMap[RetNode].addValues(Regs.at(RetNode));
  }

  for (auto Iter = Graph->return_begin(), EIter = Graph->return_end();
       Iter != EIter; ++Iter) {
    auto *RetNode = *Iter;
    if (PointsTo.count(RetNode)) {
      OutputPts[RetNode] = PointsTo.at(RetNode);
    }
  }

  collectEscapeObjs();
}

// mark all the possible stores of constant zero values
void AnalysisState::processStore(Instruction *Inst) {
  auto *StoreI = cast<StoreInst>(Inst);
  ProgramValuePtr StPtr = getNode(gvfg_utility::getPointerOperand(StoreI));
  // The actual def-use effect of a store is represented in the GVFG. The local
  // transfer only makes sure the destination pointer has a memory object so BOF
  // and null/UAF queries can still reason about the access site itself.
  initPointsToTarget(StPtr, Inst);
}

void AnalysisState::processPhiInst(Instruction *Inst) {
  auto *Dst = cast<GuardedValueFlowPhiNode>(getNode(Inst));
  for (auto Iter = Dst->begin(), EIter = Dst->end(); Iter != EIter; ++Iter) {
    const auto &InNode = *Iter;
    auto PhiCond = getPhiCond(Dst, InNode);
    assignVal(Dst, cast<GuardedValueFlowNode>(InNode.value_node), PhiCond);
  }
}

void AnalysisState::processMemAlloc(
    CallInst *Inst, const std::vector<unsigned> &AllocSizeArgs) {
  bool isArrayAlloc = AllocSizeArgs.size() > 1;
  if (!isArrayAlloc) {
    handleMalloc(Inst, AllocSizeArgs[0]);
  } else {
    handleCalloc(Inst, AllocSizeArgs[0], AllocSizeArgs[1]);
  }
}

void AnalysisState::handleMalloc(CallInst *Inst, unsigned SzIdx) {
  // Size expr is in bits
  auto *SzNode = getNode(Inst->getArgOperand(SzIdx));
  if (!Regs.count(SzNode)) {
    initSymbol(SzNode);
    if (!Regs.count(SzNode)) {
      createMemoryObject(getNode(Inst), PTItem::MK_PLACEHOLDER);
      return;
    }
  }
  auto SzVals = Regs.at(SzNode) * PropertyInteger(8);
  auto *Dst = getNode(Inst);
  for (const auto &P : SzVals) {
    auto Sz = P.first;
    if (Sz < int64_t(0)) {
      continue;
    }
    createMemoryObject(Dst, PTItem::MK_CONCRETE, P.first, P.second);
  }

  initPointsToTarget(Dst, Inst);
}

void AnalysisState::handleCalloc(CallInst *Inst, unsigned NumIdx,
                                 unsigned SzIdx) {
  auto *NumNode = getNode(Inst->getArgOperand(NumIdx));
  auto *SzNode = getNode(Inst->getArgOperand(SzIdx));
  if (!Regs.count(NumNode)) {
    initSymbol(NumNode);
  }
  if (!Regs.count(SzNode)) {
    initSymbol(SzNode);
  }
  if (!Regs.count(NumNode) || !Regs.count(SzNode)) {
    createMemoryObject(getNode(Inst), PTItem::MK_PLACEHOLDER);
    return;
  }

  auto SzVals = Regs.at(NumNode) * PropertyInteger(8) * Regs.at(SzNode);
  auto *Dst = getNode(Inst);
  for (const auto &P : SzVals) {
    createMemoryObject(Dst, PTItem::MK_CONCRETE, P.first, P.second);
  }

  // fall back, e.g., size could equal x*y where SzVals is empty.
  initPointsToTarget(Dst, Inst);
}

void AnalysisState::processFreeCall(CallInst *Inst) {
  if (Inst->arg_size() < 1)
    return;

  Value *Ptr = Inst->getArgOperand(0);
  auto *PtrNode = getNode(Ptr);
  Condition BBCond = getLocalCond(Inst->getParent());

  // Track both the SSA pointer passed to free and every allocation object it
  // may reference. Double-free checks care about repeated frees of the same
  // value, while UAF checks usually need the freed allocation site.
  FreedPtrSet.addValue(PtrNode, BBCond);

  // Also mark all points-to targets as freed
  if (hasPts(PtrNode)) {
    const PtsSet &Pts = getPts(PtrNode);
    Pts.forEach([&](const PTItem &Pt, const Condition &Cond) {
      auto AllocSite = Pt.getAllocSite();
      auto CombinedCond = BBCond && Cond;
      FreedPtrSet.addValue(AllocSite, CombinedCond);
      FreedPointers[AllocSite] = CombinedCond;
    });
  }
}

void AnalysisState::processLibraryCall(CallInst *Inst) {
  Function *Callee = Inst->getCalledFunction();
  if (!Callee) {
    return;
  }

  if (!Callee->isDeclaration()) {
    return;
  }

  // Declarations are handled by a small set of explicit models. Anything not
  // recognized here either has no state effect that matters to this analysis,
  // or is handed to the conservative unknown-library fallback below.
  // Handle free() calls for UAF and Double-Free detection
  if (isFreeLikeFunction(Callee)) {
    if (BugTy & BUG_TY_DOUBLE_FREE) {
      buildDoubleFreeQuery(Inst);
    }
    processFreeCall(Inst);
    return;
  }

  GuardedValueFlowNode *Dst = nullptr;
  if (!Inst->getType()->isVoidTy()) {
    Dst = getNode(Inst);
  }

  auto getRegsSafe = [&](GuardedValueFlowNode *Node,
                         const char *CtxTag) -> GuardedSymbolicValSet & {
    (void)CtxTag;
    if (!Node) {
      return Regs[ProgramValuePtr()]; // vacuous slot
    }
    if (!Regs.count(Node)) {
      initSymbol(Node);
    }
    if (!Regs.count(Node)) {
      return Regs[ProgramValuePtr(Node)]; // create empty entry to avoid throw
    }
    return Regs.at(Node);
  };

  if (Callee->isIntrinsic()) {
    auto IntrinsicID = Callee->getIntrinsicID();

    if (IntrinsicID == Intrinsic::expect) { // compiled from __builtin_expect
      // `llvm.expect` carries branch-likelihood metadata only, so preserve
      // the value unchanged and let the normal condition machinery decide
      // feasibility elsewhere.
      if (Inst->arg_size() >= 1) {
        auto *Arg0 = getNode(Inst->getArgOperand(0));
        bool hasArg0 = Regs.count(Arg0);
        if (!hasArg0) {
          initSymbol(Arg0);
        }
        Regs[Dst].setValues(getRegsSafe(Arg0, "expect"));
      }
    } else if (IntrinsicID == Intrinsic::umul_with_overflow) {
      // The full struct result is not modeled precisely. We keep the product
      // because that is the part used by current integer-range clients, and
      // fall back to a placeholder object if the operands are unavailable.
      auto *Arg0 = getNode(Inst->getArgOperand(0));
      auto *Arg1 = getNode(Inst->getArgOperand(1));
      bool has0 = Regs.count(Arg0);
      bool has1 = Regs.count(Arg1);
      if (!has0) {
        initSymbol(Arg0);
      }
      if (!has1) {
        initSymbol(Arg1);
      }
      if (!Regs.count(Arg0) || !Regs.count(Arg1)) {
        createMemoryObject(Dst, PTItem::MK_PLACEHOLDER);
        return;
      }
      auto Res =
          getRegsSafe(Arg0, "umul_arg0") * getRegsSafe(Arg1, "umul_arg1");
      if (!Res.empty()) {
        Regs[Dst] = std::move(Res);
      }
    }
  } else {
    Condition BBCond = getLocalCond(Inst->getParent());
    auto *MemSpec = gvfg_utility::getMemSpec();
    auto handleStringLen = [&](const ProgramValuePtr &Ptr, bool IsDirect) {
      StrState.handleCStrLen(Ptr, Inst, BBCond, *this, IsDirect);
    };

    if (gvfg_utility::isMatchLib(Inst, Callee->getName().str(), "strlen")) {
      auto *Ptr = getNode(Inst->getArgOperand(0));
      handleStringLen(Ptr, true);
      return;
    }

    if (gvfg_utility::isMatchLib(Inst, Callee->getName().str(), "puts")) {
      auto *Ptr = getNode(Inst->getArgOperand(0));
      handleStringLen(Ptr, false);
      return;
    }

    if (gvfg_utility::isMatchLib(Inst, Callee->getName().str(), "strcpy") ||
        gvfg_utility::isMatchLib(Inst, Callee->getName().str(), "strcat") ||
        gvfg_utility::isMatchLib(Inst, Callee->getName().str(), "strncat")) {
      handleStringLen(getNode(Inst->getArgOperand(0)), false);
      handleStringLen(getNode(Inst->getArgOperand(1)), false);
      return;
    }

    // Handle free() calls for UAF and Double-Free detection
    std::string CalleeName = Callee->getName().str();
    if (CalleeName == "free" || CalleeName == "_ZdlPv" ||
        CalleeName == "_ZdaPv") {
      processFreeCall(Inst);
      return;
    }

    if (MemSpec->isPureLib(Callee) ||
        gvfg_utility::isKnownLib(Callee->getName().str())) {
      return;
    }

    // For other unknown library calls
    processAsUnknownLib(Inst);
  }
}

void AnalysisState::processAsUnknownLib(CallInst *Inst) {
  for (unsigned Idx = 0; Idx < Inst->arg_size(); ++Idx) {
    Value *Arg = Inst->getArgOperand(Idx);
    auto *ArgTy = dyn_cast<PointerType>(Arg->getType());
    if (ArgTy) {
      // heuristic: if the arg points to a structure, do not
      // invalidate all the contents of the structure. Otherwise, we can
      // miss bugs.
      if (ArgTy->getPointerElementType()->isAggregateType()) {
        continue;
      }

      // Unknown calls are allowed to read or clobber non-aggregate pointees.
      // Initializing their abstract targets here makes that side effect visible
      // to later checks without exploding every field-sensitive aggregate.
      initPointsToTarget(getNode(Arg), Inst);
    }
  }

  if (Inst->getType()->isPointerTy()) {
    // A pointer result from an opaque call is treated as a fresh unknown region
    // rather than aliasing any specific incoming object.
    initPointsToTarget(getNode(Inst), Inst);
  }
}

Condition AnalysisState::getLocalCond(BasicBlock *BB) const {
  // return LocalCondMap.count(BB) ? LocalCondMap.at(BB) : Condition();
  if (LocalCondMap.count(BB)) {
    return LocalCondMap.at(BB);
  } else {
    // Local control conditions are memoized because the same block guard is
    // reused by queries, taint propagation, and free tracking. Call-site output
    // constraints are conjoined so summarized callees stay linked to the actual
    // values returned at this instantiation site.
    Condition BBCond(Solver->getCtrlDeps(BB, Graph), Solver.get());

    // FIXME
    Condition CSDeps = getCallSiteOutDeps();
    BBCond = BBCond && CSDeps;

    LocalCondMap.insert(std::make_pair(BB, BBCond));
    return BBCond;
  }
}

Condition AnalysisState::getDataDepsCond(GuardedValueFlowNode *N) const {
  if (DataDepsCondMap.count(N)) {
    return DataDepsCondMap.at(N);
  } else {
    // Data-dependence conditions capture the symbolic guards attached to a GVFG
    // producer. They are threaded into pointer arithmetic and summary mapping
    // so imported facts stay valid only when their defining values are valid.
    Condition CurDeps(Solver->getDataDeps(N), Solver.get());
    CurDeps = CurDeps && getCallSiteOutDeps();
    DataDepsCondMap.insert(std::make_pair(N, CurDeps));
    return CurDeps;
  }
}

Condition AnalysisState::getCallSiteOutDeps() const {
  auto UsedCSOuts = Solver->getUsedCallSiteOutput();
  Condition ResCond = Condition::getTrueCond();
  // Summary instantiation introduces call-site output variables. Whenever one
  // appears in a cached condition, tie it back to the concrete symbolic values
  // observed at this caller so imported path constraints remain meaningful.
  for (auto *CSO : UsedCSOuts) {
    if (CSOutputCondMap.count(CSO)) {
      Condition CurCond = CSOutputCondMap.at(CSO);
      ResCond = ResCond && CurCond;
      continue;
    }

    if (Regs.count(CSO)) {
      Condition CurCond(
          buildVarEqValues(CSO, Solver->getExpr(Var(CSO)), Regs.at(CSO)),
          Solver.get());
      CSOutputCondMap.insert(std::make_pair(CSO, CurCond));

      ResCond = ResCond && CurCond;
    }
  }
  return ResCond;
}

Condition AnalysisState::getRegionCond(GuardedValueFlowRegionNode *R) const {
  if (RegionCondMap.count(R)) {
    return RegionCondMap.at(R);
  } else {
    // Region nodes encode which store/load edge is active in the symbolic heap.
    // Caching the resulting condition avoids rebuilding the same SMT fragment
    // for every load, query, and summary export that touches the region.
    Condition Cond(R, getSolver());
    // FIXME
    Cond = Cond && getCallSiteOutDeps();
    RegionCondMap.insert(std::make_pair(R, Cond));
    return Cond;
  }
}

Condition AnalysisState::getPhiCond(
    const GuardedValueFlowPhiNode *PhiNode,
    const GuardedValueFlowPhiNode::Incoming InNode) const {
  // Phi guards are path predicates supplied by the GVFG builder. We only add
  // call-site output constraints here, not new control structure, so summary
  // imports can reuse the same incoming-edge semantics as intraprocedural flow.
  auto PhiCond = Solver->getPhiGated(PhiNode, InNode);
  auto ResCond = Condition(PhiCond, Solver.get());
  // FIXME
  ResCond = ResCond && getCallSiteOutDeps();
  return ResCond;
}

GuardedSymbolicValSet AnalysisState::inlineExpr(const PropertyValuePtr &E,
                                                Instruction *CS) const {
  if (IsaProperty<PropertyInteger>(E)) {
    return E;
  }

  if (InlineExprCache.count(E) && InlineExprCache.at(E).count(CS)) {
    return InlineExprCache.at(E).at(CS);
  }

  auto Res = evalExpr(E, FormalToRealSymValsMap.at(CS), CS);
  InlineExprCache[E][CS] = Res;
  return Res;
}

ProgramValuePtr AnalysisState::getFreeVar(Type *Ty) const {
  std::string Name =
      "free_" + gvfg_utility::ptrToString(F) + "_" + std::to_string(FreeVarID++);
  return {Ty, Name};
}

GuardedSymbolicValSet AnalysisState::evalExpr(
    const PropertyValuePtr &E,
    const std::unordered_map<ProgramValuePtr, GuardedSymbolicValSet> &M,
    Instruction *CS) const {
  if (IsaProperty<PropertyInteger>(E)) {
    return E;
  }

  // `PropertySymExpr` is an affine template over program variables. Evaluating
  // it substitutes each variable with the guarded values known in `M`; when a
  // binding is missing we materialize a fresh free variable so summaries remain
  // sound instead of silently dropping that dimension.
  std::vector<GuardedSymbolicValSet> FreeVarStorage;
  GuardedSymbolicValSet Result;
  std::vector<
      std::tuple<GuardedSymbolicValSet::IterType,
                 GuardedSymbolicValSet::IterType, BigInteger, ProgramValuePtr>>
      Seq;
  const auto *SymE = CastProperty<PropertySymExpr>(E);

  PropertyValuePtr Residual = GetProperty<PropertyInteger>(SymE->getOffsets());
  for (auto Iter = SymE->begin(), EIter = SymE->end(); Iter != EIter; ++Iter) {
    ProgramValuePtr X = Iter->first.getValue();
    BigInteger C = Iter->second;

    // C * X ===> C * M.at(X)
    if (M.count(X) && !M.at(X).empty()) {
      const auto &ValSet = M.at(X);
      Seq.emplace_back(std::make_tuple(ValSet.begin(), ValSet.end(), C, X));
    } else {
      FreeVarStorage.emplace_back(
          GetProperty<PropertySymExpr>(Var(getFreeVar(X.getType())), C));
      const auto &ValSet = FreeVarStorage.back();
      Seq.emplace_back(std::make_tuple(ValSet.begin(), ValSet.end(), C, X));
    }
  }

  if (Seq.empty()) {
    Result.addValue(Residual);
  } else {
    std::string Suffix;
    if (CS) {
      Suffix = getCallsiteSuffix(CS);
    }
    Result.addValues(
        GuardedSymbolicValSet::seqAdd(Seq, Solver.get(), true, Suffix) +
        Residual);
  }

  return Result;
}

std::string AnalysisState::getCallsiteSuffix(Instruction *CS) const {
  return "_" + gvfg_utility::ptrToString(CS);
}

SMTExpr
AnalysisState::buildVarEqValues(const ProgramValuePtr &V, SMTExpr VExpr,
                                const GuardedSymbolicValSet &SymVals) const {
  auto LSz = VExpr.getBitVecSize();
  SMTExprVec ValVec = Solver->createEmptySMTExprVec();
  SMTExprVec ValVecAll = Solver->createEmptySMTExprVec();

  SymVals.forEach([&](const PropertyValuePtr &Val, const Condition &ValCond) {
    if (ValCond.isFalse()) {
      return;
    }

    // skip the special case "V == V"
    if (IsaProperty<PropertySymExpr>(Val) &&
        CastProperty<PropertySymExpr>(Val)->isVar() &&
        CastProperty<PropertySymExpr>(Val)->getAsVar().getValue() == V) {
      // ValVecAll.push_back(VExpr == VExpr);
      return;
    }

    SMTExpr ValCondExpr = Solver->buildBoolVal(true);
    if (!ValCond.isTrue()) {
      ValCondExpr = ValCond.getSMTConstr();
    }

    SMTExpr ValExpr = Solver->buildExprForVal(Val.get());
    assert(ValExpr.isBitVector());

    unsigned RSz = ValExpr.getBitVecSize();

    // FIXME
    if (LSz == RSz) {
      ValVec.push_back((VExpr == ValExpr) && ValCondExpr);
    } else if (LSz > RSz) {
      auto Temp = ValExpr.basic_sext(LSz - RSz);
      assert(Temp.getBitVecSize() == LSz);
      ValVec.push_back((VExpr == Temp) && ValCondExpr);
    } else { // LSz < RSz
      auto Temp = ValExpr.basic_extract(LSz - 1, 0);
      assert(Temp.getBitVecSize() == LSz);
      ValVec.push_back((VExpr == Temp) && ValCondExpr);
    }
  });

  return ValVec.toOrExpr();
}

Condition AnalysisState::getMappingCond(Instruction *CS,
                                        Function *Callee) const {
  if (MappingCondCache.count(CS)) {
    return MappingCondCache.at(CS);
  }

  // Summary import is guarded by a single mapping condition that equates each
  // callee formal with the caller-side SMT expression chosen for this callsite.
  // That condition also pulls in data dependencies for non-constant actuals so
  // the instantiated summary only fires along feasible caller paths.
  std::string CSSuffix = getCallsiteSuffix(CS);
  SMTExprVec Vec = Solver->createEmptySMTExprVec();
  std::vector<Condition> RealDataDeps;

  bool IsComplete = true;
  for (auto &Iter : SMTRenameCtxMap.at(Callee)) {
    if (!Iter.second.hasRealExpr(CS)) {
      IsComplete = false;
      continue;
    }

    ProgramValuePtr Formal(Iter.first);
    SMTExpr FormalExpr =
        Solver->renameExpr(Iter.second.getFormalExpr(), CSSuffix);

    ProgramValuePtr Real = getRealForFormal(CS, Formal);
    SMTExpr RealExpr = Iter.second.getRealExpr(CS);

    Vec.push_back(FormalExpr == RealExpr);

    if (!Real.isConstant()) {
      if (Real.isa<GuardedValueFlowNodeValue>() &&
          Real.getAs<GuardedValueFlowNodeValue>()->getNode()) {
        RealDataDeps.emplace_back(getDataDepsCond(
            Real.getAs<GuardedValueFlowNodeValue>()->getNode()));
      } else {
        auto &RealSymVals = FormalToRealSymValsMap.at(CS).at(Formal);
        auto ValCond = buildVarEqValues(Real, RealExpr, RealSymVals);
        Vec.push_back(ValCond);
      }
    }
  }

  Condition ResCond(Vec.toAndExpr(), Solver.get());
  for (const auto &DepsCond : RealDataDeps) {
    ResCond = ResCond && DepsCond;
  }

  if (IsComplete) {
    MappingCondCache.insert(std::make_pair(CS, ResCond));
  }

  return ResCond;
}

bool AnalysisState::isInstUnmodelled(Instruction *Inst) const {
  auto OpC = Inst->getOpcode();
  switch (OpC) {
  case llvm::Instruction::FMul:
  case llvm::Instruction::FAdd:
  case llvm::Instruction::FSub:
  case llvm::Instruction::And:
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
  case llvm::Instruction::Shl:
  case llvm::Instruction::LShr:
  case llvm::Instruction::AShr:
  case llvm::Instruction::FDiv:
  case llvm::Instruction::SRem:
  case llvm::Instruction::FRem:
  case llvm::Instruction::URem:
  // Other unmodelled insts
  case llvm::Instruction::ExtractElement:
  case llvm::Instruction::InsertElement:
  case llvm::Instruction::FCmp:
  case llvm::Instruction::Select: {
    return true;
  }
  default: {
    return false;
  }
  }
}

bool AnalysisState::mustBeConstantInt(const ProgramValuePtr &V) const {
  if (V.isa<GuardedValueFlowNodeValue>() &&
      isa<ConstantPointerNull>(V.getLLVMVal())) {
    return false;
  }

  if (!hasPts(V)) {
    bool IsConst = true;
    for (auto &Pair : getSymbolicVals(V)) {
      if (!IsaProperty<PropertyInteger>(Pair.first)) {
        IsConst = false;
        break;
      }
    }
    return IsConst;
  }
  return false;
}
