//===----------------------------------------------------------------------===//
//
// AnalysisState query generation and checking.
// Handles building numerical queries (BOF, DBZ), checking them against
// path conditions, and reporting bugs.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#include "Analysis/SymbolicExecution/AnalysisDriver.h"
#include "Analysis/SymbolicExecution/AnalysisState.h"
#include "Analysis/SymbolicExecution/DomTreePass.h"
#include "Analysis/SymbolicExecution/InstResolver.h"
#include "Analysis/SymbolicExecution/MemoryAPI.h"
#include "Analysis/SymbolicExecution/PropertyAllocator.h"
#include "Analysis/SymbolicExecution/PropertyInteger.h"
#include "Analysis/SymbolicExecution/PropertySym.h"
#include "Analysis/SymbolicExecution/TSDataLayout.h"
#include "Analysis/SymbolicExecution/TaintModel.h"

#include <functional>
#include <map>
#include <numeric>
#include <regex>
#include <sstream>
#include <unordered_map>

#define DEBUG_TYPE "Symex"

#ifndef MTDEBUG
#define MTDEBUG(X) LLVM_DEBUG(X)
#endif

using namespace SymbolicExecution;

// Helper function to normalize condition by stripping callsite suffixes
// Suffixes are typically of the form "_0x[0-9a-f]+" (pointer addresses)
static std::string normalizeConditionForGrouping(const Condition &Cond) {
  if (Cond.isLit()) {
    return Cond.isTrue() ? "true" : "false";
  }

  // Convert condition to SMT string representation
  std::ostringstream OSS;
  OSS << Cond.getSMTConstr();
  std::string CondStr = OSS.str();

  // Strip callsite suffix patterns: "_0x" followed by hex digits
  // Pattern matches: _0x[0-9a-fA-F]+
  std::regex SuffixPattern(R"(_0x[0-9a-fA-F]+)");
  std::string Normalized = std::regex_replace(CondStr, SuffixPattern, "");

  return Normalized;
}

extern cl::opt<bool> AnalyzeNonArrayAlloca;
extern cl::opt<std::string> SymexDebugFunName;

namespace {

bool isFreeLikeFunction(const Function *callee) {
  if (!callee) {
    return false;
  }

  StringRef name = callee->getName();
  return name == "free" || name == "_ZdlPv" || name == "_ZdaPv";
}

bool needsCheck(const SymbolicExecution::PTItem &Pt) {
  if (Pt.isPlaceHolder()) {
    return false;
  }

  const auto &AllocSite = Pt.getAllocSite();
  if (Pt.isConcrete()) {
    if (AllocSite.isa<GuardedValueFlowNodeValue>() &&
        isa<AllocaInst>(AllocSite.getLLVMVal())) {
      auto *AllocI = cast<AllocaInst>(AllocSite.getLLVMVal());
      auto *AllocedTy = AllocI->getAllocatedType();

      // The cases we care about:
      // 1. %ar = alloca [100 x i32]
      // 2. %ptr = alloca i32, i32 4
      // skip checking for stack allocation that is not an array
      if (AllocedTy->isArrayTy()) {
        return true;
      } else if (AllocI->isArrayAllocation()) {
        return true;
      } else {
        if (AnalyzeNonArrayAlloca) {
          return true;
        } else {
          return false;
        }
      }
    } else {
      // always check for heap allocations.
      return true;
    }
  } else {
    assert(Pt.isSymbolic());
    if (AllocSite.isa<GuardedValueFlowNodeValue>() &&
        isa<GlobalValue>(AllocSite.getLLVMVal())) {
      return false;
    }

    // The created pseudo arg
    Type *AllocSiteTy = AllocSite.getType();
    if (!AllocSiteTy->isPointerTy()) {
      return false;
    } else {
      return true;
      // In: struct st {
      //         unsigned char *data;
      //         struct subst* p;
      //     } s;
      // Let s be the function parameter.
      // We check the access of s.data[1], but do not check for s.p[1]
      // Type *ElemTy = AllocSiteTy->getPointerElementType();

      // return ElemTy->isIntegerTy();
    }
  }
}

StringRef simplifyIntrinsicName(StringRef IntrinsicName) {
  // An intrinsic is in the form llvm.xxxx.xxx, separated by .
  // We only keep the first two sections for our internal use
  // e.g. llvm.memset.i64.i32 -> llvm.memset
  int StartIdx = IntrinsicName.find('.', 0);
  int EndIdx = IntrinsicName.find('.', StartIdx + 1);
  StringRef InternalName = IntrinsicName.substr(0, EndIdx);
  return InternalName;
}
} // namespace

/// Build queries for the current instruction if applicable (e.g., memory access
/// for BOF).
void AnalysisState::buildQuery(Instruction *Inst) {
  // This is the per-instruction dispatch point for bug querying. The executor
  // has already propagated symbolic state to Inst, so the remaining job here is
  // to recognize bug-relevant instructions and hand each one to the bug-class
  // specific encoder that knows how to turn the current abstract state into one
  // or more NumericalQuery objects.
  // The return insts should be unified.
  auto OpC = Inst->getOpcode();
  if (OpC == Instruction::Load && (BugTy & BUG_TY_BOF)) {
    auto *LoadI = cast<LoadInst>(Inst);
    buildBofQueryLoadStore(Inst, getNode(seg_utility::getPointerOperand(LoadI)),
                           LoadI->getType());
  } else if (OpC == Instruction::Store && (BugTy & BUG_TY_BOF)) {
    auto *StoreI = cast<StoreInst>(Inst);
    auto *StVal = StoreI->getValueOperand();
    buildBofQueryLoadStore(Inst,
                           getNode(seg_utility::getPointerOperand(StoreI)),
                           StVal->getType());
  } else if (isInstUnmodelled(Inst)) {
    auto *Dst = getNode(Inst);
    for (unsigned Idx = 0; Idx < Inst->getNumOperands(); ++Idx) {
      auto OpndVals = getSymbolicVals(getNode(Inst->getOperand(Idx)));
      for (const auto &P : OpndVals) {
        const auto &OpndVal = P.first;
        if (IsaProperty<PropertySymExpr>(OpndVal)) {
          for (const auto &DepV :
               CastProperty<PropertySymExpr>(OpndVal)->getUsedVars()) {
            addExtraDeps(Dst, DepV.getValue(), P.second);
          }
        }
      }
    }
  } else if (OpC == Instruction::Call) {
    if (!seg_utility::isDefiniteCall(Inst)) {
      if (BugTy & BUG_TY_BOF) {
        buildBofQueryLibCall(cast<CallInst>(Inst));
      }
    }
  }

  // URem and SRem will cause isInstUnmodelled(Inst) to return true, i.e., taint
  // passing When dbz checking is enabled, we should still generate query for
  // them.
  if (BugTy & BUG_TY_DBZ) {
    if (OpC == Instruction::SDiv || OpC == Instruction::UDiv ||
        OpC == Instruction::SRem || OpC == Instruction::URem) {
      buildDbzQuery(Inst);
    }
  }

  if (BugTy & BUG_TY_INT_OVERFLOW) {
    if (OpC == Instruction::Add || OpC == Instruction::Sub ||
        OpC == Instruction::Mul || OpC == Instruction::Shl) {
      buildIntOverflowQuery(Inst);
    }
  }

  if (BugTy & BUG_TY_INT_UNDERFLOW) {
    if (OpC == Instruction::Add || OpC == Instruction::Sub ||
        OpC == Instruction::Mul || OpC == Instruction::Shl) {
      buildIntUnderflowQuery(Inst);
    }
  }

  if (BugTy & BUG_TY_NULL_DEREF) {
    if (OpC == Instruction::Load || OpC == Instruction::Store) {
      buildNullDerefQuery(Inst);
    }
  }

  if (BugTy & BUG_TY_SIGNED_INT_OVERFLOW) {
    if (OpC == Instruction::Add || OpC == Instruction::Sub ||
        OpC == Instruction::Mul || OpC == Instruction::Shl) {
      buildSignedIntOverflowQuery(Inst);
    }
  }

  if (BugTy & BUG_TY_SIGNED_INT_UNDERFLOW) {
    if (OpC == Instruction::Add || OpC == Instruction::Sub ||
        OpC == Instruction::Mul || OpC == Instruction::Shl) {
      buildSignedIntUnderflowQuery(Inst);
    }
  }

  if (BugTy & BUG_TY_SHIFT_OVERFLOW) {
    if (OpC == Instruction::Shl || OpC == Instruction::LShr ||
        OpC == Instruction::AShr) {
      buildShiftOverflowQuery(Inst);
    }
  }

  if (BugTy & BUG_TY_ARRAY_INDEX_OOB) {
    if (OpC == Instruction::GetElementPtr) {
      buildArrayIndexOOBQuery(Inst);
    }
  }

  if (BugTy & BUG_TY_UNINIT_READ) {
    if (OpC == Instruction::Load) {
      buildUninitializedReadQuery(Inst);
    }
  }

  if (BugTy & BUG_TY_UAF) {
    if (OpC == Instruction::Load || OpC == Instruction::Store) {
      buildUafQuery(Inst);
    }
  }

  if (BugTy & BUG_TY_DOUBLE_FREE) {
    if (OpC == Instruction::Call) {
      auto *Callee = seg_utility::getCallee(Inst);
      if (!isFreeLikeFunction(Callee)) {
        buildDoubleFreeQuery(Inst);
      }
    }
  }

  if (BugTy & BUG_TY_NEGATIVE_ARRAY_INDEX) {
    if (OpC == Instruction::GetElementPtr) {
      buildNegativeArrayIndexQuery(Inst);
    }
  }

  if (BugTy & BUG_TY_INT_TRUNCATION) {
    if (OpC == Instruction::Trunc || OpC == Instruction::ZExt ||
        OpC == Instruction::SExt || OpC == Instruction::FPToSI ||
        OpC == Instruction::FPToUI) {
      buildIntTruncationQuery(Inst);
    }
  }
}

void AnalysisState::buildBofQueryLoadStore(Instruction *Inst,
                                           const ProgramValuePtr &Ptr,
                                           Type *AccTy) {
  // BOF queries are emitted from each feasible points-to target of the access.
  // The per-target condition is kept with the query trace so later summary
  // export and reporting can still tell which heap object and path made the
  // access potentially unsafe.
  const PtsSet &Pts = getPts(Ptr);
  auto AccSz =
      GetProperty<PropertyInteger>(seg_utility::getTypeSizeInBits(AccTy));

  Pts.forEach([&](const PTItem &Pt, const Condition &Cond) {
    auto Qs = createBofQuery(Pt, AccSz);
    for (const auto &Q : Qs) {
      addQueryTrace(Q, Cond,
                    TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst,
                              Ptr.getLLVMVal()));
    }
  });
}

void AnalysisState::buildDbzQuery(Instruction *Inst) {
  // DBZ uses the divisor value directly as the bug condition. Concrete zero
  // becomes an unconditional error query. Symbolic divisors become direct
  // numerical predicates that later ask whether the divisor can equal zero
  // under the current path condition and any taint requirements.
  const auto &DivisorVals = getSymbolicVals(getNode(Inst->getOperand(1)));
  DivisorVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
    if (Val == (int64_t)0) {
      auto Q = DirectNumericalQuery::getDbzMustErrQuery();
      addQueryTrace(
          Q, Cond,
          TraceStep(TraceStep::TRACE_STEP_DIV, Inst, Inst->getOperand(1)));
    } else if (IsaProperty<PropertySymExpr>(Val)) {
      auto Q = DirectNumericalQuery::getDbzSymbolicQuery(Val, getDepsVals(Val));
      addQueryTrace(
          Q, Cond,
          TraceStep(TraceStep::TRACE_STEP_DIV, Inst, Inst->getOperand(1)));
    }
  });
}

std::vector<NumericalQueryPtr>
AnalysisState::createBofQuery(const PTItem &Pt, const PropertyValuePtr &AccSz,
                              const GuardedProgramValSet &Deps) const {
  if (!needsCheck(Pt)) {
    return {};
  }

  // BOF encoding splits along the memory model. Concrete objects can often be
  // discharged immediately by comparing offset and access size with the object
  // extent. Symbolic objects keep the alloc site and arithmetic ingredients in
  // an indirect query so the same summary can be re-instantiated at callers
  // with caller-specific points-to and offset facts.
  auto Off = Pt.getOffset();
  // under read/write of a concrete memory object ==> must error
  if (Pt.isConcrete() && Off < int64_t(0)) {
    return {DirectNumericalQuery::getBofMustErrQuery()};
  }

  if (Pt.isConcrete()) {
    if (!AccSz) {
      return {DirectNumericalQuery::getBofTaintOnlyQuery(Deps)};
    }

    auto ObjSz = Pt.getSize();
    auto Diff = Off + AccSz - ObjSz;

    bool MustSafe = Diff <= int64_t(0);
    if (MustSafe) {
      return {};
    } else {
      if (IsaProperty<PropertyInteger>(Diff)) { // Must error
        return {DirectNumericalQuery::getBofMustErrQuery()};
      }

      if (IsaProperty<PropertySymExpr>(Off)) {
        // Query 1: Diff > 0
        // Query 2: 0 - Offset  > 0 (check for under read/write)
        auto MinusOff = -Off;

        auto Q1 =
            DirectNumericalQuery::getBofSymbolicQuery(Diff, getDepsVals(Diff));
        auto Q2 = DirectNumericalQuery::getBofSymbolicQuery(
            MinusOff, getDepsVals(MinusOff));

        return {Q1, Q2};
      } else {
        return {
            DirectNumericalQuery::getBofSymbolicQuery(Diff, getDepsVals(Diff))};
      }
    }
  } else {
    auto AccSzDeps = Deps;
    if (AccSz) {
      AccSzDeps = getDepsVals(AccSz);
    }

    return {IndirectNumericalQuery::getBofQuery(
        Pt.getAllocSite(), Off, AccSz, getDepsVals(Off).merge(AccSzDeps))};
  }
}

void AnalysisState::addExtraDeps(const ProgramValuePtr &Dst,
                                 const ProgramValuePtr &Src,
                                 const Condition &Cond) {
  if (Src.isConstant()) {
    return;
  }

  ExtraDepsMap[Dst].addValue(Src, Cond);
}

GuardedProgramValSet
AnalysisState::getDepsVals(const PropertyValuePtr &V) const {
  if (IsaProperty<PropertyInteger>(V)) {
    return {};
  }

  GuardedProgramValSet Res;
  const auto *SymExpr = CastProperty<PropertySymExpr>(V);
  for (const auto &UsedV : SymExpr->getUsedVars()) {
    if (ExtraDepsMap.count(UsedV.getValue())) {
      Res.addValues(ExtraDepsMap.at(UsedV.getValue()));
    }
  }
  return Res;
}

void AnalysisState::buildBofQueryLibCall(CallInst *Inst) {
  Function *Callee = Inst->getCalledFunction();
  if (!Callee) {
    return;
  }

  if (!Callee->isDeclaration()) {
    return;
  }

  StringRef FuncName = Callee->getName();
  if (Callee->isIntrinsic()) {
    FuncName = simplifyIntrinsicName(FuncName);
  }

  // Accessing the pointer Ptr with AccSzInBytes bytes.
  auto genQueryForPtrBytes = [&](const ProgramValuePtr &Ptr,
                                 const GuardedSymbolicValSet &AccSzInBytes) {
    if (hasPts(Ptr)) {
      // Change AccSize to bits
      GuardedSymbolicValSet AccSizes;
      if (AccSzInBytes.empty()) {
        AccSizes.addValue(GetProperty<PropertyInteger>(0));
      } else {
        AccSizes = AccSzInBytes * PropertyInteger(8);
      }

      const auto &Pts = getPts(Ptr);
      Pts.forEach2(AccSizes, [=](const PTItem &Pt,
                                 const PropertyValuePtr &AccSz,
                                 const Condition &Cond) {
        auto Qs = createBofQuery(Pt, AccSz);
        std::for_each(
            Qs.begin(), Qs.end(), [=](const decltype(Qs)::value_type &Q) {
              addQueryTrace(Q, Cond,
                            TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst,
                                      Ptr.getLLVMVal()));
            });
      });
    }
  };

  // when the access size is unknown, treat it as unknown but also pulls in
  // taint deps.
  auto genQueryAccSizeDep = [this, Inst](const ProgramValuePtr &Ptr,
                                         const ProgramValuePtr &Src) {
    if (hasPts(Ptr) && TaintedVals.hasValue(Src)) {
      for (const auto &P : getPts(Ptr)) {
        const auto &Pt = P.first;
        const auto &PtCond = P.second;

        auto Qs =
            createBofQuery(Pt, PropertyValuePtr(), GuardedProgramValSet(Src));
        for (const auto &Q : Qs) {
          addQueryTrace(Q, PtCond,
                        TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst,
                                  Ptr.getLLVMVal()));
        }
      }
    }
  };

  auto matchLib = [Inst, &FuncName](const std::string &Target) {
    return seg_utility::isMatchLib(Inst, FuncName.str(), Target);
  };

  // Library models reuse the same BOF query builders as regular loads and
  // stores. The difference is only how access size is reconstructed from the
  // API contract, for example byte counts, strlen-derived sizes, or tainted
  // source buffers when the size is not materialized as a direct value.
  if (matchLib("memset") || matchLib("llvm.memset")) {
    Value *Ptr = Inst->getArgOperand(0);
    Value *NumBytes = Inst->getArgOperand(2);

    genQueryForPtrBytes(getNode(Ptr), getSymbolicVals(getNode(NumBytes)));
  } else if (matchLib("snprintf")) {
    Value *Ptr = Inst->getArgOperand(0);
    Value *NumBytes = Inst->getArgOperand(1);

    genQueryForPtrBytes(getNode(Ptr), getSymbolicVals(getNode(NumBytes)));

    Value *Src = Inst->getArgOperand(2);
    genQueryAccSizeDep(getNode(Ptr), getNode(Src));
  } else if (matchLib("memcpy") || matchLib("memmove") ||
             matchLib("llvm.memcpy") || matchLib("llvm.memmove")) {
    Value *DstPtr = Inst->getArgOperand(0);
    Value *SrcPtr = Inst->getArgOperand(1);
    Value *NumBytes = Inst->getArgOperand(2);

    const auto &Sizes = getSymbolicVals(getNode(NumBytes));
    genQueryForPtrBytes(getNode(DstPtr), Sizes);
    genQueryForPtrBytes(getNode(SrcPtr), Sizes);
  } else if (matchLib("fread") || matchLib("fwrite")) {
    Value *Buffer = Inst->getArgOperand(0);
    Value *Sz = Inst->getArgOperand(1);
    Value *Count = Inst->getArgOperand(2);

    auto SzVals = getSymbolicVals(getNode(Sz));
    auto CountVals = getSymbolicVals(getNode(Count));
    genQueryForPtrBytes(getNode(Buffer), SzVals * CountVals);
  } else if (matchLib("memchr")) {
    Value *Ptr = Inst->getArgOperand(0);
    Value *NumBytes = Inst->getArgOperand(2);

    genQueryForPtrBytes(getNode(Ptr), getSymbolicVals(getNode(NumBytes)));
  } else if (matchLib("strchr")) {
    Value *Ptr = Inst->getOperand(0);
    genQueryForPtrBytes(getNode(Ptr), {});
  } else if (matchLib("strcpy")) {
    Value *DstPtr = Inst->getOperand(0);
    Value *SrcPtr = Inst->getOperand(1);
    const auto &SrcLenVals = getStrlen(getNode(SrcPtr), Inst);
    // add 1 byte for '\0'
    auto AccSizes = SrcLenVals + PropertyInteger(1);
    genQueryForPtrBytes(getNode(DstPtr), AccSizes);
    genQueryForPtrBytes(getNode(SrcPtr), {});
    genQueryAccSizeDep(getNode(DstPtr), getNode(SrcPtr));
  } else if (matchLib("puts")) {
    Value *DstPtr = Inst->getOperand(0);
    const auto &LenVals = getStrlen(getNode(DstPtr), Inst);
    // add 1 byte for '\0'
    auto AccSizes = LenVals + PropertyInteger(1);
    genQueryForPtrBytes(getNode(DstPtr), AccSizes);
  } else if (matchLib("strncpy") || matchLib("strncmp")) {
    Value *DstPtr = Inst->getOperand(0);
    Value *SrcPtr = Inst->getOperand(1);
    Value *NumBytes = Inst->getOperand(2);

    genQueryForPtrBytes(getNode(DstPtr), getSymbolicVals(getNode(NumBytes)));
    genQueryForPtrBytes(getNode(SrcPtr), getSymbolicVals(getNode(NumBytes)));
  } else if (matchLib("strstr") || matchLib("strcasestr") ||
             matchLib("strcmp")) {
    Value *S1 = Inst->getOperand(0);
    Value *S2 = Inst->getOperand(1);
    genQueryForPtrBytes(getNode(S1), {});
    genQueryForPtrBytes(getNode(S2), {});
  } else if (matchLib("strtok") || matchLib("strlen")) {
    genQueryForPtrBytes(getNode(Inst->getOperand(0)), {});
  } else if (matchLib("strncat")) {
    Value *DstPtr = Inst->getOperand(0);
    Value *SrcPtr = Inst->getOperand(1);
    Value *NumBytes = Inst->getOperand(2);

    const auto &SrcLenVals = getStrlen(getNode(SrcPtr), Inst);
    const auto &DstLenVals = getStrlen(getNode(DstPtr), Inst);
    const auto &NLenVals = getSymbolicVals(getNode(NumBytes));

    GuardedSymbolicValSet AccSizes;
    for (const auto &SrcP : SrcLenVals) {
      const auto &SrcLen = SrcP.first;
      for (const auto &DstP : DstLenVals) {
        const auto &DstLen = DstP.first;
        for (const auto &NP : NLenVals) {
          const auto &NLen = NP.first;

          PropertyValuePtr CopyLen = NLen;
          auto Diff = NLen - SrcLen;
          if (Diff > int64_t(0)) {
            CopyLen = SrcLen;
          }
          // add 1 for '\0'
          PropertyValuePtr AccSize = CopyLen + PropertyInteger(1) + DstLen;
          AccSizes.addValue(AccSize, SrcP.second && DstP.second && NP.second);
        }
      }
    }

    genQueryForPtrBytes(getNode(DstPtr), AccSizes);
    genQueryForPtrBytes(getNode(SrcPtr), {});
  } else if (matchLib("strcat")) {
    Value *DstPtr = Inst->getOperand(0);
    Value *SrcPtr = Inst->getOperand(1);

    const auto &SrcLenVals = getStrlen(getNode(SrcPtr), Inst);
    const auto &DstLenVals = getStrlen(getNode(DstPtr), Inst);
    auto AccSizes = SrcLenVals + DstLenVals + PropertyInteger(1);

    genQueryForPtrBytes(getNode(DstPtr), AccSizes);
    genQueryForPtrBytes(getNode(SrcPtr), {});
    genQueryAccSizeDep(getNode(DstPtr), getNode(SrcPtr));
  }
}

NumericalQueryPtr NumericalQuery::clone() const {
  if (QK == QK_DIRECT) {
    return std::make_shared<DirectNumericalQuery>(
        *cast<DirectNumericalQuery>(this));
  } else {
    return std::make_shared<IndirectNumericalQuery>(
        *cast<IndirectNumericalQuery>(this));
  }
}

void NumericalQuery::dumpDeps() const {
  llvm::errs() << "Deps: ";
  for (const auto &P : Deps) {
    llvm::errs() << P.first.getID() << "\n";
  }
}

NumericalQueryPtr DirectNumericalQuery::getBofMustErrQuery() {
  // The symbolic expression is resolved to a must error
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_BOF));
}

NumericalQueryPtr
DirectNumericalQuery::getBofTaintOnlyQuery(const GuardedProgramValSet &Deps) {
  // The symbolic expression could not be built, resort to taint on Deps
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_BOF, Deps));
}

NumericalQueryPtr
DirectNumericalQuery::getBofSymbolicQuery(const PropertyValuePtr &Qr,
                                          const GuardedProgramValSet &Deps) {
  // The error condition is encoded in the symbolic expr Qr +
  // we have extra taint deps in Deps
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_BOF, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getDbzMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_DBZ));
}

NumericalQueryPtr
DirectNumericalQuery::getDbzSymbolicQuery(const PropertyValuePtr &Qr,
                                          const GuardedProgramValSet &Deps) {
  // Qr == 0
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_DBZ, Qr, true, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getIntOverflowMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_INT_OVERFLOW));
}

NumericalQueryPtr DirectNumericalQuery::getIntOverflowSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_INT_OVERFLOW, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getIntUnderflowMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_INT_UNDERFLOW));
}

NumericalQueryPtr DirectNumericalQuery::getIntUnderflowSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_INT_UNDERFLOW, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getNullDerefMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_NULL_DEREF));
}

NumericalQueryPtr DirectNumericalQuery::getNullDerefSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr == 0
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_NULL_DEREF, Qr, true, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getSignedIntOverflowMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_SIGNED_INT_OVERFLOW));
}

NumericalQueryPtr DirectNumericalQuery::getSignedIntOverflowSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_SIGNED_INT_OVERFLOW, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getSignedIntUnderflowMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_SIGNED_INT_UNDERFLOW));
}

NumericalQueryPtr DirectNumericalQuery::getSignedIntUnderflowSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_SIGNED_INT_UNDERFLOW, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getShiftOverflowMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_SHIFT_OVERFLOW));
}

NumericalQueryPtr DirectNumericalQuery::getShiftOverflowSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0 (for negative shift or shift >= bitwidth)
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_SHIFT_OVERFLOW, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getArrayIndexOOBMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_ARRAY_INDEX_OOB));
}

NumericalQueryPtr DirectNumericalQuery::getArrayIndexOOBSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0 (for negative index or index >= array_size)
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_ARRAY_INDEX_OOB, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getUninitializedReadQuery() {
  // Uninitialized read is always a potential error
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_UNINIT_READ));
}

NumericalQueryPtr DirectNumericalQuery::getUafMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_UAF));
}

NumericalQueryPtr
DirectNumericalQuery::getUafSymbolicQuery(const PropertyValuePtr &Qr,
                                          const GuardedProgramValSet &Deps) {
  // Qr represents the condition that pointer points to freed memory
  // For UAF, we check if Qr is true (pointer is freed)
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_UAF, Qr, true, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getDoubleFreeMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_DOUBLE_FREE));
}

NumericalQueryPtr DirectNumericalQuery::getDoubleFreeSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr represents the condition that pointer was already freed
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_DOUBLE_FREE, Qr, true, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getNegativeArrayIndexMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_NEGATIVE_ARRAY_INDEX));
}

NumericalQueryPtr DirectNumericalQuery::getNegativeArrayIndexSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr < 0 (negative index)
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_NEGATIVE_ARRAY_INDEX, Qr, false, Deps));
}

NumericalQueryPtr DirectNumericalQuery::getIntTruncationMustErrQuery() {
  return std::shared_ptr<DirectNumericalQuery>(
      new DirectNumericalQuery(AnalysisState::BUG_TY_INT_TRUNCATION));
}

NumericalQueryPtr DirectNumericalQuery::getIntTruncationSymbolicQuery(
    const PropertyValuePtr &Qr, const GuardedProgramValSet &Deps) {
  // Qr > 0 (value exceeds destination type range)
  return std::shared_ptr<DirectNumericalQuery>(new DirectNumericalQuery(
      AnalysisState::BUG_TY_INT_TRUNCATION, Qr, false, Deps));
}

void DirectNumericalQuery::dump() const {
  llvm::errs() << "Direct Query:";
  if (mustSat()) {
    llvm::errs() << "must err!\n";
  } else if (!Q) {
    llvm::errs() << "null query expr!\n";
  } else {
    Q->dump();
  }
}

SMTExprVec DirectNumericalQuery::toSMT(PathCondSolver &Solver) const {
  SMTExprVec RetVec = Solver.createEmptySMTExprVec();
  if (!Q) {
    return RetVec;
  }

  SMTExpr ResExpr = Solver.buildExprForVal(Q.get());
  SMTExpr CstZero = Solver.buildBitVecVal(0, ResExpr.getBitVecSize());
  if (Eq) {
    RetVec.push_back(ResExpr == CstZero);
  } else {
    RetVec.push_back(ResExpr.basic_sgt(CstZero));
  }

  return RetVec;
}

NumericalQueryPtr IndirectNumericalQuery::getBofQuery(
    const ProgramValuePtr &BasePtr, const PropertyValuePtr &Offset,
    const PropertyValuePtr &AccSize, const GuardedProgramValSet &Deps) {
  return std::shared_ptr<IndirectNumericalQuery>(new IndirectNumericalQuery(
      AnalysisState::BUG_TY_BOF, BasePtr, Offset, AccSize, Deps));
}

void IndirectNumericalQuery::dump() const {
  llvm::errs() << "Indirect query:\n";
  llvm::errs() << "Base:" << BasePtr.getID() << ", Offset:";
  Offset->dump();
  llvm::errs() << "Acc Size:";
  AccSize->dump();
  dumpDeps();
}

void AnalysisState::queryProcessCall(
    Instruction *Inst, Function *Callee,
    const std::vector<std::pair<QuerySet, std::vector<TraceStep>>> &Smry) {
  // Query summaries are imported lazily at call sites. Each summarized query is
  // re-instantiated in caller terms, then extended with a call trace step so a
  // later report can explain both the sink in the callee and the call edge that
  // brought the summary into the caller.
  for (const auto &P : Smry) {
    auto Trace = P.second;
    Trace.emplace_back(TraceStep(TraceStep::TRACE_STEP_CALL, Inst, Inst));

    auto InlinedQs = inlineVals(P.first, Inst, Callee);
    // Call site condition is added within addQuery
    InlinedQs.forEach([&](const NumericalQueryPtr &Q, const Condition &Cond) {
      addQueryTrace(Q, Cond, Trace);
    });
  }
}

void AnalysisState::addQueryTrace(const NumericalQueryPtr &Q,
                                  const Condition &Cond,
                                  const TraceStep &Step) {
  addQueryTrace(Q, Cond, std::vector<TraceStep>{Step});
}

void AnalysisState::addQueryTrace(const NumericalQueryPtr &Q,
                                  const Condition &Cond,
                                  const std::vector<TraceStep> &Trace) {
  if (QueryCount >= AnalysisLimit::FUNC_QUERY_LIMIT_V) {
    return;
  }

  auto *CurSite = Trace.back().Inst;
  const auto &SiteBBCond = getLocalCond(CurSite->getParent());

  QuerySet Qs;
  if (Qs.addValue(Q, Cond && SiteBBCond)) {
    ++QueryCount;

    auto *SinkInst = Trace.front().Inst;
    auto *SinkFunc = SinkInst->getParent()->getParent();
    if (SymexDebugFunName == SinkFunc->getName()) {
      MTDEBUG(errs() << "[DEBUG-Numerical] query generated at "
                     << SinkFunc->getName() << " ");

      bool IsInlinedQuery = (Trace.size() > 1);
      if (IsInlinedQuery) {
        MTDEBUG(errs() << " inlined at "
                       << CurSite->getParent()->getParent()->getName() << "\n");
      } else {
        MTDEBUG(errs() << "\n");
      }

      MTDEBUG(SinkInst->dump());
      MTDEBUG(Q->dump());
    }

    QueryToTraces.emplace_back(std::make_pair(std::move(Qs), Trace));
  }
}

QuerySet AnalysisState::inlineQuery(const NumericalQueryPtr &Q,
                                    Instruction *Inst, Function *Callee) const {
  auto BugTy = Q->getBugTy();
  if (BugTy == BUG_TY_BOF) {
    return inlineBofQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_DBZ) {
    return inlineDbzQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_INT_OVERFLOW) {
    return inlineIntOverflowQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_INT_UNDERFLOW) {
    return inlineIntUnderflowQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_NULL_DEREF) {
    return inlineNullDerefQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_SIGNED_INT_OVERFLOW) {
    return inlineSignedIntOverflowQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_SIGNED_INT_UNDERFLOW) {
    return inlineSignedIntUnderflowQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_SHIFT_OVERFLOW) {
    return inlineShiftOverflowQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_ARRAY_INDEX_OOB) {
    return inlineArrayIndexOOBQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_UNINIT_READ) {
    return inlineUninitializedReadQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_UAF) {
    return inlineUafQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_DOUBLE_FREE) {
    return inlineDoubleFreeQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_NEGATIVE_ARRAY_INDEX) {
    return inlineNegativeArrayIndexQuery(Q, Inst, Callee);
  } else if (BugTy == BUG_TY_INT_TRUNCATION) {
    return inlineIntTruncationQuery(Q, Inst, Callee);
  }

  assert("Unsupported bug type?" && false);
  return {};
}

QuerySet AnalysisState::inlineDbzQuery(const NumericalQueryPtr &Q,
                                       Instruction *Inst,
                                       Function *Callee) const {
  const NumericalQuery *Qptr = Q.get();
  GuardedProgramValSet InlinedDeps = inlineVals(Qptr->getDeps(), Inst, Callee);

  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  if (DQ->mustSat()) {
    return DQ->clone();
  }

  QuerySet Res;
  assert(DQ->getSymExpr());
  const auto &InlinedExprs = inlineExpr(DQ->getSymExpr(), Inst);
  for (const auto &P : InlinedExprs) {
    auto Expr = P.first;
    if (IsaProperty<PropertySymExpr>(Expr)) {
      Res.addValue(DirectNumericalQuery::getDbzSymbolicQuery(Expr, InlinedDeps),
                   P.second);
    } else {
      assert(IsaProperty<PropertyInteger>(Expr));
      if (Expr == int64_t(0)) { // resolved to be error
        Res.addValue(DirectNumericalQuery::getDbzMustErrQuery(), P.second);
      }
    }
  }

  return Res;
}

QuerySet AnalysisState::inlineBofQuery(const NumericalQueryPtr &Q,
                                       Instruction *Inst,
                                       Function *Callee) const {
  const NumericalQuery *Qptr = Q.get();

  const auto &FormalToReal = FormalToRealMap.at(Inst);
  GuardedProgramValSet InlinedDeps = inlineVals(Qptr->getDeps(), Inst, Callee);

  if (isa<DirectNumericalQuery>(Qptr)) {
    const auto *DQ = cast<DirectNumericalQuery>(Qptr);
    if (DQ->mustSat()) {
      return DQ->clone();
    }

    QuerySet Res;
    // Query expr is null but the dependence is not empty
    if (!DQ->getSymExpr()) {
      Res.addValue(DirectNumericalQuery::getBofTaintOnlyQuery(InlinedDeps));
      return Res;
    }

    const auto &InlinedExprs = inlineExpr(DQ->getSymExpr(), Inst);
    for (const auto &P : InlinedExprs) {
      auto Expr = P.first;
      if (IsaProperty<PropertySymExpr>(Expr)) {
        Res.addValue(
            DirectNumericalQuery::getBofSymbolicQuery(Expr, InlinedDeps),
            P.second);
      } else {
        assert(IsaProperty<PropertyInteger>(Expr));
        if (Expr > int64_t(0)) { // resolved to be error
          Res.addValue(DirectNumericalQuery::getBofMustErrQuery(), P.second);
        }
      }
    }

    return Res;
  } else {
    // Indirect BOF summaries are the interprocedural form used when the callee
    // only knows a symbolic base object. Re-instantiation replaces that formal
    // base with the caller's points-to set, adds caller offsets and access
    // sizes, and collapses back to direct must-error or symbolic queries when a
    // concrete caller object makes the check precise enough.
    const auto *IDQ = cast<IndirectNumericalQuery>(Qptr);
    auto BasePtr = IDQ->getBasePtr();
    auto Offset = IDQ->getOffset();
    auto AccSize = IDQ->getAccSize();

    PtsSet BaseAtCaller;
    if (FormalToReal.count(BasePtr)) {
      auto Real = FormalToReal.at(BasePtr);
      if (hasPts(Real)) {
        BaseAtCaller.addValues(getPts(Real));
      } else if (mustBeConstantInt(Real)) {
        return DirectNumericalQuery::getBofMustErrQuery();
      }
    }

    if (BaseAtCaller.empty()) {
      return {};
    }

    auto InlinedOffsets = inlineExpr(Offset, Inst);
    GuardedSymbolicValSet InlinedAccSizes;

    if (AccSize) {
      InlinedAccSizes = inlineExpr(AccSize, Inst);
    }

    QuerySet Res;
    for (const auto &P : BaseAtCaller) {
      const auto &Pt = P.first;
      const auto &PtCond = P.second;
      if (!needsCheck(Pt)) {
        continue;
      }

      auto CallerOffsets = InlinedOffsets + Pt.getOffset();
      if (Pt.isSymbolic()) {
        for (const auto &OffCond : CallerOffsets) {
          const auto &Off = OffCond.first;
          if (!AccSize) {
            Res.addValue(IndirectNumericalQuery::getBofQuery(
                             Pt.getAllocSite(), Off, PropertyValuePtr(),
                             getDepsVals(Off).merge(InlinedDeps)),
                         PtCond && OffCond.second);
            if (Res.isFull()) {
              return Res;
            }
            continue;
          }

          for (const auto &AccSzCond : InlinedAccSizes) {
            const auto &AccSz = AccSzCond.first;
            Res.addValue(IndirectNumericalQuery::getBofQuery(
                             Pt.getAllocSite(), Off, AccSz,
                             getDepsVals(Off).merge(
                                 getDepsVals(AccSz).merge(InlinedDeps))),
                         PtCond && OffCond.second && AccSzCond.second);
            if (Res.isFull()) {
              return Res;
            }
          }
        }
      } else {
        assert(Pt.isConcrete());
        for (const auto &OffCond : CallerOffsets) {
          const auto &CallerOff = OffCond.first;
          // under read/write of a concrete memory object ==> must
          // error
          if (CallerOff < int64_t(0)) {
            Res.addValue(DirectNumericalQuery::getBofMustErrQuery(),
                         PtCond && OffCond.second);
            continue;
          }

          if (!AccSize) {
            Res.addValue(
                DirectNumericalQuery::getBofTaintOnlyQuery(InlinedDeps),
                PtCond && OffCond.second);
            continue;
          }

          for (const auto &AccSzCond : InlinedAccSizes) {
            auto Diff = CallerOff + AccSzCond.first - Pt.getSize();
            if (IsaProperty<PropertyInteger>(Diff)) {
              if (Diff <= int64_t(0)) { // Must Safe

              } else { // Must error
                Res.addValue(DirectNumericalQuery::getBofMustErrQuery(),
                             PtCond && OffCond.second && AccSzCond.second);
              }

            } else {
              // FIXME: Currently I igonre the other query for
              // CallerOffset < 0
              Res.addValue(DirectNumericalQuery::getBofSymbolicQuery(
                               Diff, getDepsVals(Diff).merge(InlinedDeps)),
                           PtCond && OffCond.second && AccSzCond.second);
            }

            if (Res.isFull()) {
              return Res;
            }
          }
        }
      }
    }

    return Res;
  }
}

void AnalysisState::buildQuerySummary() {
  // Always print to verify function is called
  llvm::errs() << "[MPA] buildQuerySummary() called with "
               << QueryToTraces.size() << " query entries\n";

  std::vector<std::pair<QuerySet, std::vector<TraceStep>>> RemainingQueries;

  // QueryToTraces stores one query with the trace that led to its sink. Before
  // reporting, we reorganize that stream into context-sharing batches so the
  // solver can answer many bug predicates under the same path condition in one
  // pass. Queries that still need more precise handling are kept for later
  // stages by rebuilding QueryToTraces from the unresolved subset.
  // Group queries by shared context (QDepCond) for batch checking
  struct QueryInfo {
    NumericalQueryPtr Query;
    Condition QDepCond; // Store by value, not reference
    std::vector<TraceStep> Trace;
    Function *SinkFun;
    size_t OriginalIndex;
    unsigned BugTy;
  };

  std::vector<QueryInfo> AllQueries;
  // Group by normalized condition (stripping callsite suffixes) to enable
  // batch checking of queries that differ only by callsite renaming
  std::map<std::string, std::vector<size_t>> ContextGroups;

  // Collect all queries and group by context. The normalization step strips the
  // callsite-specific suffixes that summary import adds during renaming, which
  // lets equivalent caller contexts share a batch even though their SMT names
  // differ.
  for (size_t i = 0; i < QueryToTraces.size(); ++i) {
    const auto &P = QueryToTraces[i];
    const auto &Trace = P.second;
    auto *SinkFun = Trace.front().Inst->getParent()->getParent();

    assert(P.first.size() == 1);
    for (const auto &QCond : P.first) {
      auto QPtr = QCond.first;
      const Condition &QDepCond = QCond.second;

      QueryInfo Info;
      Info.Query = QPtr;
      Info.QDepCond = QDepCond;
      Info.Trace = Trace;
      Info.SinkFun = SinkFun;
      Info.OriginalIndex = i;
      Info.BugTy = QPtr->getBugTy();

      size_t idx = AllQueries.size();
      AllQueries.push_back(Info);

      // Group by normalized condition (stripping callsite suffixes)
      std::string NormalizedKey = normalizeConditionForGrouping(QDepCond);
      ContextGroups[NormalizedKey].push_back(idx);
    }
  }

  // Always print MPA statistics (not conditional on debug flags)
  llvm::errs() << "[MPA] Total queries: " << AllQueries.size()
               << ", Context groups: " << ContextGroups.size() << "\n";

  // Process queries in batches grouped by shared context
  std::vector<bool> Resolved(AllQueries.size(), false);

  for (const auto &Group : ContextGroups) {
    const auto &Indices = Group.second;

    if (Indices.size() == 1) {
      // Single query - use regular checking
      size_t idx = Indices[0];
      const auto &Info = AllQueries[idx];
      bool Result = false;

      switch (Info.BugTy) {
      case BUG_TY_BOF:
        Result = tryReportBofQuery(Info.Query, Info.QDepCond, Info.SinkFun,
                                   Info.Trace);
        break;
      case BUG_TY_DBZ:
        Result = tryReportDbzQuery(Info.Query, Info.QDepCond, Info.SinkFun,
                                   Info.Trace);
        break;
      case BUG_TY_INT_OVERFLOW:
        Result = tryReportIntOverflowQuery(Info.Query, Info.QDepCond,
                                           Info.SinkFun, Info.Trace);
        break;
      case BUG_TY_INT_UNDERFLOW:
        Result = tryReportIntUnderflowQuery(Info.Query, Info.QDepCond,
                                            Info.SinkFun, Info.Trace);
        break;
      case BUG_TY_NULL_DEREF:
        Result = tryReportNullDerefQuery(Info.Query, Info.QDepCond,
                                         Info.SinkFun, Info.Trace);
        break;
      case BUG_TY_SIGNED_INT_OVERFLOW:
        Result = tryReportSignedIntOverflowQuery(Info.Query, Info.QDepCond,
                                                 Info.SinkFun, Info.Trace);
        break;
      case BUG_TY_SIGNED_INT_UNDERFLOW:
        Result = tryReportSignedIntUnderflowQuery(Info.Query, Info.QDepCond,
                                                  Info.SinkFun, Info.Trace);
        break;
      case BUG_TY_SHIFT_OVERFLOW:
        Result = tryReportShiftOverflowQuery(Info.Query, Info.QDepCond,
                                             Info.SinkFun, Info.Trace);
        break;
      case BUG_TY_ARRAY_INDEX_OOB:
        Result = tryReportArrayIndexOOBQuery(Info.Query, Info.QDepCond,
                                             Info.SinkFun, Info.Trace);
        break;
      case BUG_TY_UNINIT_READ:
        Result = tryReportUninitializedReadQuery(Info.Query, Info.QDepCond,
                                                 Info.SinkFun, Info.Trace);
        break;
      case BUG_TY_UAF:
        Result = tryReportUafQuery(Info.Query, Info.QDepCond, Info.SinkFun,
                                   Info.Trace);
        break;
      case BUG_TY_DOUBLE_FREE:
        Result = tryReportDoubleFreeQuery(Info.Query, Info.QDepCond,
                                          Info.SinkFun, Info.Trace);
        break;
      case BUG_TY_NEGATIVE_ARRAY_INDEX:
        Result = tryReportNegativeArrayIndexQuery(Info.Query, Info.QDepCond,
                                                  Info.SinkFun, Info.Trace);
        break;
      case BUG_TY_INT_TRUNCATION:
        Result = tryReportIntTruncationQuery(Info.Query, Info.QDepCond,
                                             Info.SinkFun, Info.Trace);
        break;
      default:
        assert(false && "Unknown bug type");
      }

      Resolved[idx] = Result;
    } else {
      // Multiple queries with same context - use batch checking
      const Condition &SharedContext = AllQueries[Indices[0]].QDepCond;
      SMTExprVec Context = SharedContext.toSMT(*Solver);

      // Always print batch checking info
      llvm::errs() << "[MPA] Checking batch with " << Indices.size()
                   << " queries sharing normalized context\n";

      // Separate unconditional bug conditions from predicate-bearing queries.
      // "Must sat" entries only need the shared context to be feasible, while
      // the others contribute an additional bug predicate on top of that
      // context.
      std::vector<size_t> MustSatIndices;
      std::vector<size_t> RegularIndices;

      for (size_t idx : Indices) {
        const auto &Info = AllQueries[idx];
        auto *DQ = dyn_cast<DirectNumericalQuery>(Info.Query.get());
        if (DQ && DQ->mustSat()) {
          MustSatIndices.push_back(idx);
        } else {
          RegularIndices.push_back(idx);
        }
      }

      // Batch check "must sat" queries (they only need context check)
      if (!MustSatIndices.empty()) {
        // llvm::errs() << "[MPA] Batch checking " << MustSatIndices.size()
        //              << " 'must sat' queries\n";
        //  For "must sat" queries, we just need to check if context is
        //  satisfiable
        bool ContextSat = Solver->isConstraintSat(Context);
        for (size_t idx : MustSatIndices) {
          const auto &Info = AllQueries[idx];
          if (ContextSat) {
            // Report bug for "must sat" queries
            BugReports.emplace_back(std::make_tuple(
                static_cast<AnalysisState::SymexBugType>(Info.BugTy),
                std::vector<TaintStep>{}, Info.Trace));
            Resolved[idx] = true;
          } else {
            Resolved[idx] = true; // Filtered (unsat context)
          }
        }
      }

      // Batch checking covers the pure numerical predicate. Taint-sensitive
      // filtering is still delegated to the existing per-bug reporting path,
      // so the optimization reduces repeated context solving without changing
      // the bug-specific report criteria.
      if (!RegularIndices.empty()) {
        // llvm::errs() << "[MPA] Batch checking " << RegularIndices.size()
        //              << " regular queries\n";

        // Build predicates for batch checking
        std::vector<SMTExprVec> Predicates;
        Predicates.reserve(RegularIndices.size());

        for (size_t idx : RegularIndices) {
          const auto &Info = AllQueries[idx];
          auto *DQ = dyn_cast<DirectNumericalQuery>(Info.Query.get());
          if (DQ && !DQ->mustSat()) {
            SMTExprVec QueryPred = DQ->toSMT(*Solver);
            Predicates.push_back(std::move(QueryPred));
          } else {
            // For indirect queries or other cases, use empty predicate
            Predicates.push_back(Solver->createEmptySMTExprVec());
          }
        }

        // Perform batch satisfiability check: Context ∧ Predicates[i]
        std::vector<bool> BatchResults =
            Solver->batchCheckPredicates(Context, Predicates);

        // Process batch results and handle taint checking individually
        for (size_t i = 0; i < RegularIndices.size(); ++i) {
          size_t idx = RegularIndices[i];
          const auto &Info = AllQueries[idx];
          bool BaseSat = BatchResults[i];

          if (!BaseSat) {
            // Base query is unsatisfiable, no need to check taint
            Resolved[idx] = true; // Filtered
            continue;
          }

          // Base query is satisfiable, now check with taint (individual check)
          // This still uses the existing tryReport*Query logic for taint
          // handling
          bool Result = false;
          switch (Info.BugTy) {
          case BUG_TY_BOF:
            Result = tryReportBofQuery(Info.Query, Info.QDepCond, Info.SinkFun,
                                       Info.Trace);
            break;
          case BUG_TY_DBZ:
            Result = tryReportDbzQuery(Info.Query, Info.QDepCond, Info.SinkFun,
                                       Info.Trace);
            break;
          case BUG_TY_INT_OVERFLOW:
            Result = tryReportIntOverflowQuery(Info.Query, Info.QDepCond,
                                               Info.SinkFun, Info.Trace);
            break;
          case BUG_TY_INT_UNDERFLOW:
            Result = tryReportIntUnderflowQuery(Info.Query, Info.QDepCond,
                                                Info.SinkFun, Info.Trace);
            break;
          case BUG_TY_NULL_DEREF:
            Result = tryReportNullDerefQuery(Info.Query, Info.QDepCond,
                                             Info.SinkFun, Info.Trace);
            break;
          case BUG_TY_SIGNED_INT_OVERFLOW:
            Result = tryReportSignedIntOverflowQuery(Info.Query, Info.QDepCond,
                                                     Info.SinkFun, Info.Trace);
            break;
          case BUG_TY_SIGNED_INT_UNDERFLOW:
            Result = tryReportSignedIntUnderflowQuery(Info.Query, Info.QDepCond,
                                                      Info.SinkFun, Info.Trace);
            break;
          case BUG_TY_SHIFT_OVERFLOW:
            Result = tryReportShiftOverflowQuery(Info.Query, Info.QDepCond,
                                                 Info.SinkFun, Info.Trace);
            break;
          case BUG_TY_ARRAY_INDEX_OOB:
            Result = tryReportArrayIndexOOBQuery(Info.Query, Info.QDepCond,
                                                 Info.SinkFun, Info.Trace);
            break;
          case BUG_TY_UNINIT_READ:
            Result = tryReportUninitializedReadQuery(Info.Query, Info.QDepCond,
                                                     Info.SinkFun, Info.Trace);
            break;
          case BUG_TY_UAF:
            Result = tryReportUafQuery(Info.Query, Info.QDepCond, Info.SinkFun,
                                       Info.Trace);
            break;
          case BUG_TY_DOUBLE_FREE:
            Result = tryReportDoubleFreeQuery(Info.Query, Info.QDepCond,
                                              Info.SinkFun, Info.Trace);
            break;
          case BUG_TY_NEGATIVE_ARRAY_INDEX:
            Result = tryReportNegativeArrayIndexQuery(Info.Query, Info.QDepCond,
                                                      Info.SinkFun, Info.Trace);
            break;
          case BUG_TY_INT_TRUNCATION:
            Result = tryReportIntTruncationQuery(Info.Query, Info.QDepCond,
                                                 Info.SinkFun, Info.Trace);
            break;
          default:
            assert(false && "Unknown bug type");
          }

          Resolved[idx] = Result;
        }
      }
    }
  }

  // Collect unresolved queries
  for (size_t i = 0; i < AllQueries.size(); ++i) {
    if (!Resolved[i]) {
      const auto &Info = AllQueries[i];
      QuerySet Qs;
      Qs.addValue(Info.Query, Info.QDepCond);
      RemainingQueries.emplace_back(std::make_pair(Qs, Info.Trace));
    }
  }

  QueryToTraces = std::move(RemainingQueries);
}

bool AnalysisState::tryReportDbzQuery(const NumericalQueryPtr &QPtr,
                                      const Condition &QDepCond,
                                      Function *SinkFun,
                                      const std::vector<TraceStep> &Trace) {

  bool Reported = false, Filtered = false;
  auto *Q = cast<DirectNumericalQuery>(QPtr.get());
  if (Q->mustSat()) {
    SMTExprVec DepConstr = QDepCond.toSMT(*Solver);
    if (Solver->isConstraintSat(DepConstr)) {
      if (SymexDebugFunName == SinkFun->getName()) {
        MTDEBUG(llvm::errs() << "Query must err, adding bug report in func "
                             << F->getName() << "\n");
        std::string DepConstrStr;
        DepConstr.SMTExprVecToStream(DepConstrStr);
        MTDEBUG(llvm::errs() << "Sat query constr: " << DepConstrStr << "\n");
      }

      BugReports.emplace_back(std::make_tuple(
          static_cast<AnalysisState::SymexBugType>(QPtr->getBugTy()),
          std::vector<TaintStep>{}, Trace));
      Reported = true;
    } else {
      if (SymexDebugFunName == SinkFun->getName()) {
        MTDEBUG(llvm::errs() << "Query must err, filtered bug report in func "
                             << F->getName() << "\n");
        std::string DepConstrStr;
        DepConstr.SMTExprVecToStream(DepConstrStr);
        MTDEBUG(llvm::errs() << "Unsat query constr: " << DepConstrStr << "\n");
      }
      Filtered = true;
    }
  } else {
    std::vector<TaintStep> TaintSteps;
    bool QueryConsBuilt = false;
    SMTExprVec QueryCons = Solver->createEmptySMTExprVec();

    for (const auto &VCond : Q->getUsedVals()) {
      auto V = VCond.first;
      bool Tainted = TaintedVals.hasValue(V);
      if (Tainted) {
        if (TaintedSteps.count(V)) {
          TaintSteps = TaintedSteps.at(V);
        }

        auto TaintCond = TaintedVals.getGuardForValue(V) && VCond.second;
        SMTExprVec TaintCons = TaintCond.toSMT(*Solver);

        if (!QueryConsBuilt) {
          SMTExprVec DepConstr = QDepCond.toSMT(*Solver);
          QueryCons = cast<DirectNumericalQuery>(Q)->toSMT(*Solver);
          QueryCons.mergeWithAnd(DepConstr);

          QueryConsBuilt = true;
        }
        TaintCons.mergeWithAnd(QueryCons);

        if (SymexDebugFunName == SinkFun->getName()) {
          MTDEBUG(llvm::errs() << "Try reporting query: \n");
          MTDEBUG(Q->dump());
          MTDEBUG(llvm::errs() << "QueryCons: \n");
          std::string QueryConsStr;
          QueryCons.SMTExprVecToStream(QueryConsStr);
          MTDEBUG(llvm::errs() << QueryConsStr << "\n");
          MTDEBUG(llvm::errs() << "FinalCons: \n");
          std::string TaintConsStr;
          TaintCons.SMTExprVecToStream(TaintConsStr);
          MTDEBUG(llvm::errs() << TaintConsStr << "\n");
        }

        if (Solver->isConstraintSat(TaintCons)) {
          if (SymexDebugFunName == SinkFun->getName()) {
            MTDEBUG(llvm::errs() << "Query added to bug report in func "
                                 << F->getName() << "\n");
          }

          BugReports.emplace_back(std::make_tuple(
              static_cast<AnalysisState::SymexBugType>(Q->getBugTy()),
              TaintSteps, Trace));
          Reported = true;
          break;
        } else {
          if (SymexDebugFunName == SinkFun->getName()) {
            MTDEBUG(llvm::errs() << "Query filtered due to unsat!\n");
          }

          Filtered = true;
        }
      }
    }
  }

  return Reported || Filtered;
}

bool AnalysisState::tryReportBofQuery(const NumericalQueryPtr &QPtr,
                                      const Condition &QDepCond,
                                      Function *SinkFun,
                                      const std::vector<TraceStep> &Trace) {
  bool Reported = false, Filtered = false;
  auto *Q = QPtr.get();

  if (isa<DirectNumericalQuery>(Q) &&
      cast<DirectNumericalQuery>(Q)->mustSat()) {

    SMTExprVec DepConstr = QDepCond.toSMT(*Solver);
    if (Solver->isConstraintSat(DepConstr)) {
      if (SymexDebugFunName == SinkFun->getName()) {
        MTDEBUG(llvm::errs() << "Query must err, adding bug report in func "
                             << F->getName() << "\n");
        std::string DepConstrStr;
        DepConstr.SMTExprVecToStream(DepConstrStr);
        MTDEBUG(llvm::errs() << "Sat query constr: " << DepConstrStr << "\n");
      }

      BugReports.emplace_back(std::make_tuple(
          static_cast<AnalysisState::SymexBugType>(QPtr->getBugTy()),
          std::vector<TaintStep>{}, Trace));
      Reported = true;
    } else {
      if (SymexDebugFunName == SinkFun->getName()) {
        MTDEBUG(llvm::errs() << "Query must err, filtered bug report in func "
                             << F->getName() << "\n");
        std::string DepConstrStr;
        DepConstr.SMTExprVecToStream(DepConstrStr);
        MTDEBUG(llvm::errs() << "Unsat query constr: " << DepConstrStr << "\n");
      }
      Filtered = true;
    }
  } else if (isa<DirectNumericalQuery>(Q) ||
             (isa<IndirectNumericalQuery>(Q) &&
              seg_utility::isFunctionTopLevel(F))) {
    std::vector<TaintStep> TaintSteps;

    bool QueryConsBuilt = false;
    SMTExprVec QueryCons = Solver->createEmptySMTExprVec();

    for (const auto &VCond : Q->getUsedVals()) {
      auto V = VCond.first;
      bool Tainted = TaintedVals.hasValue(V);
      if (Tainted) {
        if (TaintedSteps.count(V)) {
          TaintSteps = TaintedSteps.at(V);
        }
        auto TaintCond = TaintedVals.getGuardForValue(V) && VCond.second;
        SMTExprVec TaintCons = TaintCond.toSMT(*Solver);

        if (!QueryConsBuilt) {
          SMTExprVec DepConstr = QDepCond.toSMT(*Solver);
          if (isa<DirectNumericalQuery>(Q)) {
            QueryCons = cast<DirectNumericalQuery>(Q)->toSMT(*Solver);
            QueryCons.mergeWithAnd(DepConstr);
          } else {
            QueryCons = DepConstr;
          }

          QueryConsBuilt = true;
        }

        TaintCons.mergeWithAnd(QueryCons);

        if (SymexDebugFunName == SinkFun->getName()) {
          MTDEBUG(llvm::errs() << "Try reporting query: \n");
          MTDEBUG(Q->dump());
          MTDEBUG(llvm::errs() << "QueryCons: \n");
          std::string QueryConsStr;
          QueryCons.SMTExprVecToStream(QueryConsStr);
          MTDEBUG(llvm::errs() << QueryConsStr << "\n");
          MTDEBUG(llvm::errs() << "FinalCons: \n");
          std::string TaintConsStr;
          TaintCons.SMTExprVecToStream(TaintConsStr);
          MTDEBUG(llvm::errs() << TaintConsStr << "\n");
        }

        if (Solver->isConstraintSat(TaintCons)) {
          if (SymexDebugFunName == SinkFun->getName()) {
            MTDEBUG(llvm::errs() << "Query added to bug report in func "
                                 << F->getName() << "\n");
          }

          BugReports.emplace_back(std::make_tuple(
              static_cast<AnalysisState::SymexBugType>(Q->getBugTy()),
              TaintSteps, Trace));
          Reported = true;
          break;
        } else {
          if (SymexDebugFunName == SinkFun->getName()) {
            MTDEBUG(llvm::errs() << "Query filtered due to unsat!\n");
          }

          Filtered = true;
        }
      }
    }
  }

  return Reported || Filtered;
}

static std::pair<BigInteger, BigInteger> getMinMaxValues(Type *Ty) {
  unsigned BitWidth = Ty->getIntegerBitWidth();
  BigInteger One(1);
  APInt Shifted = One.getVal().shl(BitWidth - 1);
  BigInteger Min = -BigInteger(Shifted);
  BigInteger Max = BigInteger(Shifted) - One;
  return {Min, Max};
}

void AnalysisState::buildIntOverflowQuery(Instruction *Inst) {
  if (!Inst->getType()->isIntegerTy())
    return;

  auto Op1Vals = getSymbolicVals(getNode(Inst->getOperand(0)));
  auto Op2Vals = getSymbolicVals(getNode(Inst->getOperand(1)));
  auto MinMax = getMinMaxValues(Inst->getType());
  auto MaxVal = PropertyInteger(MinMax.second);

  GuardedSymbolicValSet ResVals;
  if (Inst->getOpcode() == Instruction::Add) {
    ResVals = Op1Vals + Op2Vals;
  } else if (Inst->getOpcode() == Instruction::Sub) {
    ResVals = Op1Vals - Op2Vals;
  } else if (Inst->getOpcode() == Instruction::Mul) {
    ResVals = Op1Vals * Op2Vals;
  } else {
    return;
  }

  ResVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
    if (IsaProperty<PropertyInteger>(Val)) {
      // Constant check
      auto IntVal = CastProperty<PropertyInteger>(Val)->getVal();
      if (IntVal > MinMax.second) {
        auto Q = DirectNumericalQuery::getIntOverflowMustErrQuery();
        addQueryTrace(Q, Cond,
                      TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst));
      }
    } else if (IsaProperty<PropertySymExpr>(Val)) {
      // Val - Max > 0
      auto Qr = Val - MaxVal;
      auto Q = DirectNumericalQuery::getIntOverflowSymbolicQuery(
          Qr, getDepsVals(Qr));
      addQueryTrace(Q, Cond,
                    TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst));
    }
  });
}

void AnalysisState::buildIntUnderflowQuery(Instruction *Inst) {
  if (!Inst->getType()->isIntegerTy())
    return;

  auto Op1Vals = getSymbolicVals(getNode(Inst->getOperand(0)));
  auto Op2Vals = getSymbolicVals(getNode(Inst->getOperand(1)));
  auto MinMax = getMinMaxValues(Inst->getType());
  auto MinVal = PropertyInteger(MinMax.first);

  GuardedSymbolicValSet ResVals;
  if (Inst->getOpcode() == Instruction::Add) {
    ResVals = Op1Vals + Op2Vals;
  } else if (Inst->getOpcode() == Instruction::Sub) {
    ResVals = Op1Vals - Op2Vals;
  } else if (Inst->getOpcode() == Instruction::Mul) {
    ResVals = Op1Vals * Op2Vals;
  } else {
    return;
  }

  ResVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
    if (IsaProperty<PropertyInteger>(Val)) {
      // Constant check
      auto IntVal = CastProperty<PropertyInteger>(Val)->getVal();
      if (IntVal < MinMax.first) {
        auto Q = DirectNumericalQuery::getIntUnderflowMustErrQuery();
        addQueryTrace(Q, Cond,
                      TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst));
      }
    } else if (IsaProperty<PropertySymExpr>(Val)) {
      // Min - Val > 0
      PropertyValuePtr MinValPtr(GetProperty<PropertyInteger>(MinMax.first));
      auto Qr = MinValPtr - Val;
      auto Q = DirectNumericalQuery::getIntUnderflowSymbolicQuery(
          Qr, getDepsVals(Qr));
      addQueryTrace(Q, Cond,
                    TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst));
    }
  });
}

void AnalysisState::buildSignedIntOverflowQuery(Instruction *Inst) {
  if (!Inst->getType()->isIntegerTy())
    return;

  auto Op1Vals = getSymbolicVals(getNode(Inst->getOperand(0)));
  auto Op2Vals = getSymbolicVals(getNode(Inst->getOperand(1)));

  // Get signed min/max values (same as getMinMaxValues but explicitly for
  // signed)
  auto MinMax = getMinMaxValues(Inst->getType());
  auto MaxVal = PropertyInteger(MinMax.second);

  GuardedSymbolicValSet ResVals;
  if (Inst->getOpcode() == Instruction::Add) {
    ResVals = Op1Vals + Op2Vals;
  } else if (Inst->getOpcode() == Instruction::Sub) {
    ResVals = Op1Vals - Op2Vals;
  } else if (Inst->getOpcode() == Instruction::Mul) {
    ResVals = Op1Vals * Op2Vals;
  } else {
    // Shl is handled separately in buildShiftOverflowQuery
    return;
  }

  ResVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
    if (IsaProperty<PropertyInteger>(Val)) {
      auto IntVal = CastProperty<PropertyInteger>(Val)->getVal();
      if (IntVal > MinMax.second) {
        auto Q = DirectNumericalQuery::getSignedIntOverflowMustErrQuery();
        addQueryTrace(Q, Cond,
                      TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst));
      }
    } else if (IsaProperty<PropertySymExpr>(Val)) {
      // Val - Max > 0
      auto Qr = Val - MaxVal;
      auto Q = DirectNumericalQuery::getSignedIntOverflowSymbolicQuery(
          Qr, getDepsVals(Qr));
      addQueryTrace(Q, Cond,
                    TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst));
    }
  });
}

void AnalysisState::buildSignedIntUnderflowQuery(Instruction *Inst) {
  if (!Inst->getType()->isIntegerTy())
    return;

  auto Op1Vals = getSymbolicVals(getNode(Inst->getOperand(0)));
  auto Op2Vals = getSymbolicVals(getNode(Inst->getOperand(1)));

  // Get signed min/max values
  auto MinMax = getMinMaxValues(Inst->getType());
  auto MinVal = PropertyInteger(MinMax.first);

  GuardedSymbolicValSet ResVals;
  if (Inst->getOpcode() == Instruction::Add) {
    ResVals = Op1Vals + Op2Vals;
  } else if (Inst->getOpcode() == Instruction::Sub) {
    ResVals = Op1Vals - Op2Vals;
  } else if (Inst->getOpcode() == Instruction::Mul) {
    ResVals = Op1Vals * Op2Vals;
  } else {
    // Shl is handled separately in buildShiftOverflowQuery
    return;
  }

  ResVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
    if (IsaProperty<PropertyInteger>(Val)) {
      auto IntVal = CastProperty<PropertyInteger>(Val)->getVal();
      if (IntVal < MinMax.first) {
        auto Q = DirectNumericalQuery::getSignedIntUnderflowMustErrQuery();
        addQueryTrace(Q, Cond,
                      TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst));
      }
    } else if (IsaProperty<PropertySymExpr>(Val)) {
      // Min - Val > 0
      PropertyValuePtr MinValPtr(GetProperty<PropertyInteger>(MinMax.first));
      auto Qr = MinValPtr - Val;
      auto Q = DirectNumericalQuery::getSignedIntUnderflowSymbolicQuery(
          Qr, getDepsVals(Qr));
      addQueryTrace(Q, Cond,
                    TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst));
    }
  });
}

void AnalysisState::buildShiftOverflowQuery(Instruction *Inst) {
  if (!Inst->getType()->isIntegerTy())
    return;

  auto ShiftAmountVals = getSymbolicVals(getNode(Inst->getOperand(1)));
  unsigned BitWidth = Inst->getType()->getIntegerBitWidth();
  auto MaxShift = GetProperty<PropertyInteger>(BitWidth);
  auto Zero = GetProperty<PropertyInteger>(0);

  ShiftAmountVals.forEach(
      [&](const PropertyValuePtr &Val, const Condition &Cond) {
        if (IsaProperty<PropertyInteger>(Val)) {
          auto IntVal = CastProperty<PropertyInteger>(Val)->getVal();
          // Check for negative shift or shift >= bitwidth
          if (IntVal < 0 || IntVal >= BitWidth) {
            auto Q = DirectNumericalQuery::getShiftOverflowMustErrQuery();
            addQueryTrace(Q, Cond,
                          TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst));
          }
        } else if (IsaProperty<PropertySymExpr>(Val)) {
          // Check: Val < 0 || Val >= BitWidth
          // This is: Val < 0 || (Val - BitWidth) >= 0
          // We create two queries: one for negative, one for overflow
          PropertyValuePtr ZeroPtr = Zero;
          PropertyValuePtr MaxShiftPtr = MaxShift;
          auto NegQr = ZeroPtr - Val; // 0 - Val > 0 means Val < 0
          auto OverflowQr =
              Val - MaxShiftPtr; // Val - BitWidth >= 0 means Val >= BitWidth

          auto NegQ = DirectNumericalQuery::getShiftOverflowSymbolicQuery(
              NegQr, getDepsVals(NegQr));
          auto OverflowQ = DirectNumericalQuery::getShiftOverflowSymbolicQuery(
              OverflowQr, getDepsVals(OverflowQr));

          addQueryTrace(NegQ, Cond,
                        TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst));
          addQueryTrace(OverflowQ, Cond,
                        TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Inst));
        }
      });
}

void AnalysisState::buildArrayIndexOOBQuery(Instruction *Inst) {
  auto *GEP = dyn_cast<GetElementPtrInst>(Inst);
  if (!GEP)
    return;

  Value *BasePtr = GEP->getPointerOperand();
  const PtsSet &Pts = getPts(getNode(BasePtr));

  // Get the offset from GEP
  // For GEP, we need to calculate the total offset
  PropertyValuePtr TotalOffset = GetProperty<PropertyInteger>(0);
  for (unsigned i = 1; i < GEP->getNumOperands(); ++i) {
    auto IndexVals = getSymbolicVals(getNode(GEP->getOperand(i)));
    // For simplicity, we'll use the first value
    // In a more complete implementation, we'd handle all values
    if (!IndexVals.empty()) {
      auto FirstVal = IndexVals.begin()->first;
      // This is a simplified version - real implementation would need
      // to handle type sizes properly
      TotalOffset = TotalOffset + FirstVal;
    }
  }

  Pts.forEach([&](const PTItem &Pt, const Condition &Cond) {
    if (!needsCheck(Pt))
      return;

    auto ObjSz = Pt.getSize();
    auto Off = Pt.getOffset();

    // Check if offset + access goes beyond bounds
    // For array indexing, we check if index < 0 or index >= array_size
    if (Pt.isConcrete()) {
      // Check negative index
      if (Off < int64_t(0)) {
        auto Q = DirectNumericalQuery::getArrayIndexOOBMustErrQuery();
        addQueryTrace(
            Q, Cond,
            TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, BasePtr));
        return;
      }

      // Check positive overflow
      if (ObjSz && Off >= ObjSz) {
        auto Q = DirectNumericalQuery::getArrayIndexOOBMustErrQuery();
        addQueryTrace(
            Q, Cond,
            TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, BasePtr));
        return;
      }
    }

    // For symbolic offsets, create queries
    if (IsaProperty<PropertySymExpr>(Off)) {
      // Query 1: Off < 0 (negative index)
      PropertyValuePtr ZeroPtr = GetProperty<PropertyInteger>(0);
      auto NegQr = ZeroPtr - Off;
      auto NegQ = DirectNumericalQuery::getArrayIndexOOBSymbolicQuery(
          NegQr, getDepsVals(NegQr));
      addQueryTrace(
          NegQ, Cond,
          TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, BasePtr));

      // Query 2: Off >= ObjSz (positive overflow)
      if (ObjSz && IsaProperty<PropertyInteger>(ObjSz)) {
        auto OverflowQr = Off - ObjSz;
        auto OverflowQ = DirectNumericalQuery::getArrayIndexOOBSymbolicQuery(
            OverflowQr, getDepsVals(OverflowQr));
        addQueryTrace(
            OverflowQ, Cond,
            TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, BasePtr));
      }
    }
  });
}

void AnalysisState::buildUninitializedReadQuery(Instruction *Inst) {
  auto *LoadI = dyn_cast<LoadInst>(Inst);
  if (!LoadI)
    return;

  Value *Ptr = seg_utility::getPointerOperand(LoadI);
  const PtsSet &Pts = getPts(getNode(Ptr));

  // Track which memory locations have been written to
  // For now, we'll use a simple approach: check if the memory location
  // has been written to before this load
  // In a more complete implementation, we'd track writes per memory object

  Pts.forEach([&](const PTItem &, const Condition &Cond) {
    // Check if this memory location has been initialized
    // This is a simplified check - in reality, we'd need to track
    // all stores to this memory location
    // For now, we'll create a query that can be checked later
    auto Q = DirectNumericalQuery::getUninitializedReadQuery();
    addQueryTrace(Q, Cond,
                  TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, Ptr));
  });
}

void AnalysisState::buildNullDerefQuery(Instruction *Inst) {
  Value *Ptr = nullptr;
  if (LoadInst *LoadI = dyn_cast<LoadInst>(Inst)) {
    Ptr = seg_utility::getPointerOperand(LoadI);
  } else if (StoreInst *StoreI = dyn_cast<StoreInst>(Inst)) {
    Ptr = seg_utility::getPointerOperand(StoreI);
  }
  if (!Ptr)
    return;

  auto PtrVals = getSymbolicVals(getNode(Ptr));
  PtrVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
    if (Val == (int64_t)0) {
      auto Q = DirectNumericalQuery::getNullDerefMustErrQuery();
      addQueryTrace(Q, Cond, TraceStep(TraceStep::TRACE_STEP_DEREF, Inst, Ptr));
    } else if (IsaProperty<PropertySymExpr>(Val)) {
      auto Q = DirectNumericalQuery::getNullDerefSymbolicQuery(
          Val, getDepsVals(Val));
      addQueryTrace(Q, Cond, TraceStep(TraceStep::TRACE_STEP_DEREF, Inst, Ptr));
    }
  });
}

void AnalysisState::buildUafQuery(Instruction *Inst) {
  Value *Ptr = nullptr;
  if (LoadInst *LoadI = dyn_cast<LoadInst>(Inst)) {
    Ptr = seg_utility::getPointerOperand(LoadI);
  } else if (StoreInst *StoreI = dyn_cast<StoreInst>(Inst)) {
    Ptr = seg_utility::getPointerOperand(StoreI);
  }
  if (!Ptr)
    return;

  auto *PtrNode = getNode(Ptr);

  // First check if the pointer itself was freed
  if (FreedPtrSet.hasValue(PtrNode)) {
    auto FreeCond = FreedPtrSet.getGuardForValue(PtrNode);
    if (!FreeCond.isFalse()) {
      auto Q = DirectNumericalQuery::getUafMustErrQuery();
      addQueryTrace(Q, FreeCond,
                    TraceStep(TraceStep::TRACE_STEP_DEREF, Inst, Ptr));
    }
  }

  // Also check if any of the points-to targets are in the freed set
  if (hasPts(PtrNode)) {
    const PtsSet &Pts = getPts(PtrNode);
    Pts.forEach([&](const PTItem &Pt, const Condition &Cond) {
      auto AllocSite = Pt.getAllocSite();

      // Check if this allocation site was freed
      if (FreedPtrSet.hasValue(AllocSite)) {
        auto FreeCond = FreedPtrSet.getGuardForValue(AllocSite);
        auto CombinedCond = Cond && FreeCond;

        if (!CombinedCond.isFalse()) {
          // Create a query: pointer points to freed memory
          auto Q = DirectNumericalQuery::getUafMustErrQuery();
          addQueryTrace(Q, CombinedCond,
                        TraceStep(TraceStep::TRACE_STEP_DEREF, Inst, Ptr));
        }
      }
    });
  }
}

void AnalysisState::buildDoubleFreeQuery(Instruction *Inst) {
  auto *CallI = dyn_cast<CallInst>(Inst);
  if (!CallI)
    return;

  Function *Callee = seg_utility::getCallee(Inst);
  if (!Callee)
    return;

  if (!isFreeLikeFunction(Callee))
    return;

  if (CallI->arg_size() < 1)
    return;

  Value *Ptr = CallI->getArgOperand(0);
  auto *PtrNode = getNode(Ptr);

  bool has_double_free = false;
  Condition double_free_cond = Condition::getFalseCond();

  if (FreedPtrSet.hasValue(PtrNode)) {
    has_double_free = true;
    double_free_cond = FreedPtrSet.getGuardForValue(PtrNode);
  }

  if (hasPts(PtrNode)) {
    const PtsSet &Pts = getPts(PtrNode);
    Pts.forEach([&](const PTItem &Pt, const Condition &Cond) {
      auto AllocSite = Pt.getAllocSite();
      if (!FreedPtrSet.hasValue(AllocSite)) {
        return;
      }

      Condition CombinedCond = Cond && FreedPtrSet.getGuardForValue(AllocSite);
      if (CombinedCond.isFalse()) {
        return;
      }

      if (has_double_free) {
        double_free_cond.orCond(CombinedCond);
      } else {
        has_double_free = true;
        double_free_cond = CombinedCond;
      }
    });
  }

  if (has_double_free && !double_free_cond.isFalse()) {
    auto Q = DirectNumericalQuery::getDoubleFreeMustErrQuery();
    addQueryTrace(Q, double_free_cond,
                  TraceStep(TraceStep::TRACE_STEP_CALL, Inst, Ptr));
  }
}

void AnalysisState::buildNegativeArrayIndexQuery(Instruction *Inst) {
  auto *GEP = dyn_cast<GetElementPtrInst>(Inst);
  if (!GEP)
    return;

  // Check each index operand for negative values
  for (unsigned i = 1; i < GEP->getNumOperands(); ++i) {
    Value *Idx = GEP->getOperand(i);
    auto IdxVals = getSymbolicVals(getNode(Idx));

    IdxVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
      if (Val < int64_t(0)) {
        // Index is definitely negative
        auto Q = DirectNumericalQuery::getNegativeArrayIndexMustErrQuery();
        addQueryTrace(
            Q, Cond, TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, Idx));
      } else if (IsaProperty<PropertySymExpr>(Val)) {
        // Check if index can be negative symbolically
        // Create query: index < 0, which is equivalent to 0 - index > 0
        PropertyValuePtr Zero(GetProperty<PropertyInteger>(0));
        auto NegIdx = Zero - Val;
        auto Q = DirectNumericalQuery::getNegativeArrayIndexSymbolicQuery(
            NegIdx, getDepsVals(Val));
        addQueryTrace(
            Q, Cond, TraceStep(TraceStep::TRACE_STEP_BUFFER_ACCESS, Inst, Idx));
      }
    });
  }
}

void AnalysisState::buildIntTruncationQuery(Instruction *Inst) {
  Value *Src = Inst->getOperand(0);
  Type *SrcTy = Src->getType();
  Type *DstTy = Inst->getType();

  // Only check integer type conversions
  if (!SrcTy->isIntegerTy() || !DstTy->isIntegerTy())
    return;

  unsigned SrcBitWidth = SrcTy->getIntegerBitWidth();
  unsigned DstBitWidth = DstTy->getIntegerBitWidth();

  // Only check if truncating (destination is smaller)
  if (DstBitWidth >= SrcBitWidth)
    return;

  auto SrcVals = getSymbolicVals(getNode(Src));

  // Get min/max values for destination type
  auto DstMinMax = getMinMaxValues(DstTy);
  BigInteger DstMin = DstMinMax.first;
  BigInteger DstMax = DstMinMax.second;

  SrcVals.forEach([&](const PropertyValuePtr &Val, const Condition &Cond) {
    if (IsaProperty<PropertyInteger>(Val)) {
      BigInteger ValInt = CastProperty<PropertyInteger>(Val)->getAsBoundInt();
      if (ValInt < DstMin || ValInt > DstMax) {
        // Value definitely exceeds destination type range
        auto Q = DirectNumericalQuery::getIntTruncationMustErrQuery();
        addQueryTrace(Q, Cond,
                      TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Src));
      }
    } else if (IsaProperty<PropertySymExpr>(Val)) {
      // Check symbolically if value can exceed destination type range
      // Query: (value > max) || (value < min)
      // We'll check both conditions separately
      PropertyValuePtr MaxVal(
          GetProperty<PropertyInteger>(DstMax.getAsBoundInt()));
      PropertyValuePtr MinVal(
          GetProperty<PropertyInteger>(DstMin.getAsBoundInt()));

      // Check upper bound: value - max > 0
      auto UpperBound = Val - MaxVal;
      if (IsaProperty<PropertySymExpr>(UpperBound) ||
          (IsaProperty<PropertyInteger>(UpperBound) &&
           UpperBound > int64_t(0))) {
        auto Q = DirectNumericalQuery::getIntTruncationSymbolicQuery(
            UpperBound, getDepsVals(Val));
        addQueryTrace(Q, Cond,
                      TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Src));
      }

      // Check lower bound: min - value > 0
      auto LowerBound = MinVal - Val;
      if (IsaProperty<PropertySymExpr>(LowerBound) ||
          (IsaProperty<PropertyInteger>(LowerBound) &&
           LowerBound > int64_t(0))) {
        auto Q = DirectNumericalQuery::getIntTruncationSymbolicQuery(
            LowerBound, getDepsVals(Val));
        addQueryTrace(Q, Cond,
                      TraceStep(TraceStep::TRACE_STEP_ARITH, Inst, Src));
      }
    }
  });
}

bool AnalysisState::tryReportIntOverflowQuery(
    const NumericalQueryPtr &Q, const Condition &Cond, Function *SinkFun,
    const std::vector<TraceStep> &Trace) {
  // Same logic as BOF (Inequality > 0)
  return tryReportBofQuery(Q, Cond, SinkFun, Trace);
}

bool AnalysisState::tryReportIntUnderflowQuery(
    const NumericalQueryPtr &Q, const Condition &Cond, Function *SinkFun,
    const std::vector<TraceStep> &Trace) {
  // Same logic as BOF (Inequality > 0)
  return tryReportBofQuery(Q, Cond, SinkFun, Trace);
}

bool AnalysisState::tryReportNullDerefQuery(
    const NumericalQueryPtr &Q, const Condition &Cond, Function *SinkFun,
    const std::vector<TraceStep> &Trace) {
  // Same logic as DBZ (Equality == 0)
  return tryReportDbzQuery(Q, Cond, SinkFun, Trace);
}

QuerySet AnalysisState::inlineIntOverflowQuery(const NumericalQueryPtr &Q,
                                               Instruction *Inst,
                                               Function *Callee) const {
  // Similar to Bof
  const NumericalQuery *Qptr = Q.get();
  GuardedProgramValSet InlinedDeps = inlineVals(Qptr->getDeps(), Inst, Callee);

  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  if (DQ->mustSat()) {
    return DQ->clone();
  }

  QuerySet Res;
  assert(DQ->getSymExpr());
  const auto &InlinedExprs = inlineExpr(DQ->getSymExpr(), Inst);
  for (const auto &P : InlinedExprs) {
    auto Expr = P.first;
    if (IsaProperty<PropertySymExpr>(Expr)) {
      Res.addValue(
          DirectNumericalQuery::getIntOverflowSymbolicQuery(Expr, InlinedDeps),
          P.second);
    } else {
      assert(IsaProperty<PropertyInteger>(Expr));
      if (Expr > int64_t(0)) { // resolved to be error
        Res.addValue(DirectNumericalQuery::getIntOverflowMustErrQuery(),
                     P.second);
      }
    }
  }

  return Res;
}

QuerySet AnalysisState::inlineIntUnderflowQuery(const NumericalQueryPtr &Q,
                                                Instruction *Inst,
                                                Function *Callee) const {
  // Similar to Bof
  const NumericalQuery *Qptr = Q.get();
  GuardedProgramValSet InlinedDeps = inlineVals(Qptr->getDeps(), Inst, Callee);

  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  if (DQ->mustSat()) {
    return DQ->clone();
  }

  QuerySet Res;
  assert(DQ->getSymExpr());
  const auto &InlinedExprs = inlineExpr(DQ->getSymExpr(), Inst);
  for (const auto &P : InlinedExprs) {
    auto Expr = P.first;
    if (IsaProperty<PropertySymExpr>(Expr)) {
      Res.addValue(
          DirectNumericalQuery::getIntUnderflowSymbolicQuery(Expr, InlinedDeps),
          P.second);
    } else {
      assert(IsaProperty<PropertyInteger>(Expr));
      if (Expr > int64_t(0)) { // resolved to be error
        Res.addValue(DirectNumericalQuery::getIntUnderflowMustErrQuery(),
                     P.second);
      }
    }
  }

  return Res;
}

QuerySet AnalysisState::inlineNullDerefQuery(const NumericalQueryPtr &Q,
                                             Instruction *Inst,
                                             Function *Callee) const {
  // Similar to Dbz
  const NumericalQuery *Qptr = Q.get();
  GuardedProgramValSet InlinedDeps = inlineVals(Qptr->getDeps(), Inst, Callee);

  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  if (DQ->mustSat()) {
    return DQ->clone();
  }

  QuerySet Res;
  assert(DQ->getSymExpr());
  const auto &InlinedExprs = inlineExpr(DQ->getSymExpr(), Inst);
  for (const auto &P : InlinedExprs) {
    auto Expr = P.first;
    if (IsaProperty<PropertySymExpr>(Expr)) {
      Res.addValue(
          DirectNumericalQuery::getNullDerefSymbolicQuery(Expr, InlinedDeps),
          P.second);
    } else {
      assert(IsaProperty<PropertyInteger>(Expr));
      if (Expr == int64_t(0)) { // resolved to be error
        Res.addValue(DirectNumericalQuery::getNullDerefMustErrQuery(),
                     P.second);
      }
    }
  }

  return Res;
}

// Signed integer overflow/underflow inline and tryReport methods
QuerySet AnalysisState::inlineSignedIntOverflowQuery(const NumericalQueryPtr &Q,
                                                     Instruction *Inst,
                                                     Function *Callee) const {
  // Similar to IntOverflow
  const NumericalQuery *Qptr = Q.get();
  GuardedProgramValSet InlinedDeps = inlineVals(Qptr->getDeps(), Inst, Callee);

  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  if (DQ->mustSat()) {
    return DQ->clone();
  }

  QuerySet Res;
  assert(DQ->getSymExpr());
  const auto &InlinedExprs = inlineExpr(DQ->getSymExpr(), Inst);
  for (const auto &P : InlinedExprs) {
    auto Expr = P.first;
    if (IsaProperty<PropertySymExpr>(Expr)) {
      Res.addValue(DirectNumericalQuery::getSignedIntOverflowSymbolicQuery(
                       Expr, InlinedDeps),
                   P.second);
    } else {
      assert(IsaProperty<PropertyInteger>(Expr));
      if (Expr > int64_t(0)) { // resolved to be error
        Res.addValue(DirectNumericalQuery::getSignedIntOverflowMustErrQuery(),
                     P.second);
      }
    }
  }

  return Res;
}

QuerySet AnalysisState::inlineSignedIntUnderflowQuery(
    const NumericalQueryPtr &Q, Instruction *Inst, Function *Callee) const {
  // Similar to IntUnderflow
  const NumericalQuery *Qptr = Q.get();
  GuardedProgramValSet InlinedDeps = inlineVals(Qptr->getDeps(), Inst, Callee);

  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  if (DQ->mustSat()) {
    return DQ->clone();
  }

  QuerySet Res;
  assert(DQ->getSymExpr());
  const auto &InlinedExprs = inlineExpr(DQ->getSymExpr(), Inst);
  for (const auto &P : InlinedExprs) {
    auto Expr = P.first;
    if (IsaProperty<PropertySymExpr>(Expr)) {
      Res.addValue(DirectNumericalQuery::getSignedIntUnderflowSymbolicQuery(
                       Expr, InlinedDeps),
                   P.second);
    } else {
      assert(IsaProperty<PropertyInteger>(Expr));
      if (Expr > int64_t(0)) { // resolved to be error
        Res.addValue(DirectNumericalQuery::getSignedIntUnderflowMustErrQuery(),
                     P.second);
      }
    }
  }

  return Res;
}

QuerySet AnalysisState::inlineShiftOverflowQuery(const NumericalQueryPtr &Q,
                                                 Instruction *Inst,
                                                 Function *Callee) const {
  // Similar to IntOverflow
  const NumericalQuery *Qptr = Q.get();
  GuardedProgramValSet InlinedDeps = inlineVals(Qptr->getDeps(), Inst, Callee);

  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  if (DQ->mustSat()) {
    return DQ->clone();
  }

  QuerySet Res;
  assert(DQ->getSymExpr());
  const auto &InlinedExprs = inlineExpr(DQ->getSymExpr(), Inst);
  for (const auto &P : InlinedExprs) {
    auto Expr = P.first;
    if (IsaProperty<PropertySymExpr>(Expr)) {
      Res.addValue(DirectNumericalQuery::getShiftOverflowSymbolicQuery(
                       Expr, InlinedDeps),
                   P.second);
    } else {
      assert(IsaProperty<PropertyInteger>(Expr));
      if (Expr > int64_t(0)) { // resolved to be error
        Res.addValue(DirectNumericalQuery::getShiftOverflowMustErrQuery(),
                     P.second);
      }
    }
  }

  return Res;
}

QuerySet AnalysisState::inlineArrayIndexOOBQuery(const NumericalQueryPtr &Q,
                                                 Instruction *Inst,
                                                 Function *Callee) const {
  // Similar to Bof
  const NumericalQuery *Qptr = Q.get();
  GuardedProgramValSet InlinedDeps = inlineVals(Qptr->getDeps(), Inst, Callee);

  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  if (DQ->mustSat()) {
    return DQ->clone();
  }

  QuerySet Res;
  assert(DQ->getSymExpr());
  const auto &InlinedExprs = inlineExpr(DQ->getSymExpr(), Inst);
  for (const auto &P : InlinedExprs) {
    auto Expr = P.first;
    if (IsaProperty<PropertySymExpr>(Expr)) {
      Res.addValue(DirectNumericalQuery::getArrayIndexOOBSymbolicQuery(
                       Expr, InlinedDeps),
                   P.second);
    } else {
      assert(IsaProperty<PropertyInteger>(Expr));
      if (Expr > int64_t(0)) { // resolved to be error
        Res.addValue(DirectNumericalQuery::getArrayIndexOOBMustErrQuery(),
                     P.second);
      }
    }
  }

  return Res;
}

QuerySet AnalysisState::inlineUninitializedReadQuery(const NumericalQueryPtr &Q,
                                                     Instruction *Inst,
                                                     Function *Callee) const {
  (void)Inst;
  (void)Callee;
  // Uninitialized read is always a potential error
  const NumericalQuery *Qptr = Q.get();
  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  return DQ->clone();
}

bool AnalysisState::tryReportSignedIntOverflowQuery(
    const NumericalQueryPtr &Q, const Condition &Cond, Function *SinkFun,
    const std::vector<TraceStep> &Trace) {
  // Same logic as BOF (Inequality > 0)
  return tryReportBofQuery(Q, Cond, SinkFun, Trace);
}

bool AnalysisState::tryReportSignedIntUnderflowQuery(
    const NumericalQueryPtr &Q, const Condition &Cond, Function *SinkFun,
    const std::vector<TraceStep> &Trace) {
  // Same logic as BOF (Inequality > 0)
  return tryReportBofQuery(Q, Cond, SinkFun, Trace);
}

bool AnalysisState::tryReportShiftOverflowQuery(
    const NumericalQueryPtr &Q, const Condition &Cond, Function *SinkFun,
    const std::vector<TraceStep> &Trace) {
  // Same logic as BOF (Inequality > 0)
  return tryReportBofQuery(Q, Cond, SinkFun, Trace);
}

bool AnalysisState::tryReportArrayIndexOOBQuery(
    const NumericalQueryPtr &Q, const Condition &Cond, Function *SinkFun,
    const std::vector<TraceStep> &Trace) {
  // Same logic as BOF (Inequality > 0)
  return tryReportBofQuery(Q, Cond, SinkFun, Trace);
}

bool AnalysisState::tryReportUninitializedReadQuery(
    const NumericalQueryPtr &Q, const Condition &Cond, Function *SinkFun,
    const std::vector<TraceStep> &Trace) {
  (void)SinkFun;
  // Uninitialized read is always a potential error - report if constraints are
  // satisfiable
  bool Reported = false;
  auto *DQ = cast<DirectNumericalQuery>(Q.get());
  if (DQ->mustSat()) {
    SMTExprVec DepConstr = Cond.toSMT(*Solver);
    if (Solver->isConstraintSat(DepConstr)) {
      BugReports.emplace_back(std::make_tuple(
          static_cast<AnalysisState::SymexBugType>(Q->getBugTy()),
          std::vector<TaintStep>(), Trace));
      Reported = true;
    }
  }
  return Reported;
}

QuerySet AnalysisState::inlineUafQuery(const NumericalQueryPtr &Q,
                                       Instruction *Inst,
                                       Function *Callee) const {
  // Similar to null deref - just clone the query
  const NumericalQuery *Qptr = Q.get();
  GuardedProgramValSet InlinedDeps = inlineVals(Qptr->getDeps(), Inst, Callee);
  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  if (DQ->mustSat()) {
    return DQ->clone();
  }
  QuerySet Res;
  if (DQ->getSymExpr()) {
    const auto &InlinedExprs = inlineExpr(DQ->getSymExpr(), Inst);
    for (const auto &P : InlinedExprs) {
      auto Expr = P.first;
      if (IsaProperty<PropertySymExpr>(Expr)) {
        Res.addValue(
            DirectNumericalQuery::getUafSymbolicQuery(Expr, InlinedDeps),
            P.second);
      } else {
        assert(IsaProperty<PropertyInteger>(Expr));
        if (Expr == int64_t(0)) {
          Res.addValue(DirectNumericalQuery::getUafMustErrQuery(), P.second);
        }
      }
    }
  }
  return Res;
}

QuerySet AnalysisState::inlineDoubleFreeQuery(const NumericalQueryPtr &Q,
                                              Instruction *Inst,
                                              Function *Callee) const {
  (void)Inst;
  (void)Callee;
  // Similar to UAF - just clone the query
  const NumericalQuery *Qptr = Q.get();
  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  return DQ->clone();
}

QuerySet AnalysisState::inlineNegativeArrayIndexQuery(
    const NumericalQueryPtr &Q, Instruction *Inst, Function *Callee) const {
  // Similar to array index OOB
  const NumericalQuery *Qptr = Q.get();
  GuardedProgramValSet InlinedDeps = inlineVals(Qptr->getDeps(), Inst, Callee);
  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  if (DQ->mustSat()) {
    return DQ->clone();
  }
  QuerySet Res;
  assert(DQ->getSymExpr());
  const auto &InlinedExprs = inlineExpr(DQ->getSymExpr(), Inst);
  for (const auto &P : InlinedExprs) {
    auto Expr = P.first;
    if (IsaProperty<PropertySymExpr>(Expr)) {
      Res.addValue(DirectNumericalQuery::getNegativeArrayIndexSymbolicQuery(
                       Expr, InlinedDeps),
                   P.second);
    } else {
      assert(IsaProperty<PropertyInteger>(Expr));
      if (Expr > int64_t(0)) {
        Res.addValue(DirectNumericalQuery::getNegativeArrayIndexMustErrQuery(),
                     P.second);
      }
    }
  }
  return Res;
}

QuerySet AnalysisState::inlineIntTruncationQuery(const NumericalQueryPtr &Q,
                                                 Instruction *Inst,
                                                 Function *Callee) const {
  // Similar to integer overflow
  const NumericalQuery *Qptr = Q.get();
  GuardedProgramValSet InlinedDeps = inlineVals(Qptr->getDeps(), Inst, Callee);
  const auto *DQ = cast<DirectNumericalQuery>(Qptr);
  if (DQ->mustSat()) {
    return DQ->clone();
  }
  QuerySet Res;
  assert(DQ->getSymExpr());
  const auto &InlinedExprs = inlineExpr(DQ->getSymExpr(), Inst);
  for (const auto &P : InlinedExprs) {
    auto Expr = P.first;
    if (IsaProperty<PropertySymExpr>(Expr)) {
      Res.addValue(DirectNumericalQuery::getIntTruncationSymbolicQuery(
                       Expr, InlinedDeps),
                   P.second);
    } else {
      assert(IsaProperty<PropertyInteger>(Expr));
      if (Expr > int64_t(0)) {
        Res.addValue(DirectNumericalQuery::getIntTruncationMustErrQuery(),
                     P.second);
      }
    }
  }
  return Res;
}

bool AnalysisState::tryReportUafQuery(const NumericalQueryPtr &Q,
                                      const Condition &Cond, Function *SinkFun,
                                      const std::vector<TraceStep> &Trace) {
  // Similar logic to null deref (equality check)
  return tryReportDbzQuery(Q, Cond, SinkFun, Trace);
}

bool AnalysisState::tryReportDoubleFreeQuery(
    const NumericalQueryPtr &Q, const Condition &Cond, Function *SinkFun,
    const std::vector<TraceStep> &Trace) {
  // Similar logic to null deref (equality check)
  return tryReportDbzQuery(Q, Cond, SinkFun, Trace);
}

bool AnalysisState::tryReportNegativeArrayIndexQuery(
    const NumericalQueryPtr &Q, const Condition &Cond, Function *SinkFun,
    const std::vector<TraceStep> &Trace) {
  // Similar logic to BOF (inequality > 0, but for negative values)
  return tryReportBofQuery(Q, Cond, SinkFun, Trace);
}

bool AnalysisState::tryReportIntTruncationQuery(
    const NumericalQueryPtr &Q, const Condition &Cond, Function *SinkFun,
    const std::vector<TraceStep> &Trace) {
  // Similar logic to BOF (inequality > 0)
  return tryReportBofQuery(Q, Cond, SinkFun, Trace);
}
