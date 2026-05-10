#include "Dataflow/VASCO/Analyses/LLVMPointsToAnalysis.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Operator.h>

namespace vasco {
namespace llvmir {

namespace {

MemoryLocationSet singletonLocation(const MemoryBlock &Object,
                                    std::int64_t Offset = 0) {
  return {MemoryLocation::exact(Object, Offset)};
}

MemoryLocationSet summaryLocation() {
  return {MemoryLocation::summary(MemoryBlock::summary())};
}

} // namespace

PointsToAnalysis::PointsToAnalysis(
    const ProgramRepresentation<MethodType, NodeType> &Program)
    : Program(Program) {
  this->setFreeResultsOnTheFly(false);
}

PointsToAnalysis::DomainType
PointsToAnalysis::boundaryValue(const MethodType &EntryPoint) {
  DomainType EntryValue;
  if (EntryPoint == nullptr) {
    return EntryValue;
  }

  const std::size_t ContextId = 0;
  for (auto &Arg : EntryPoint->args()) {
    if (!isPointerLike(&Arg)) {
      continue;
    }
    const auto Object = classifyObject(&Arg, ContextId);
    EntryValue.assign(PointsToValue::forValue(&Arg), singletonLocation(Object));
  }

  seedGlobals(EntryValue);
  return EntryValue;
}

PointsToAnalysis::DomainType PointsToAnalysis::meet(const DomainType &LHS,
                                                    const DomainType &RHS) {
  DomainType Result = LHS;
  Result.unionWith(RHS);
  return Result;
}

std::optional<PointsToAnalysis::DomainType>
PointsToAnalysis::flowFunction(ContextPtr Context, const NodeType &Node,
                               const DomainType &InValue) {
  DomainType OutValue = copy(InValue);
  const auto ContextId = Context != nullptr ? Context->getId() : 0;

  if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Node)) {
    OutValue.assign(PointsToValue::forValue(Alloca),
                    singletonLocation(classifyObject(Alloca, ContextId)));
    return OutValue;
  }

  if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Node)) {
    const auto *DL = dataLayout();
    if (DL != nullptr) {
      MemoryModel Model(*DL);
      // realloc has both alloc and copy semantics so it requires special
      // handling separate from the generic alloc/copy classification.
      if (handleReallocCall(*Call, OutValue, InValue, ContextId)) {
        return OutValue;
      }
      const auto Summary = Model.classifyExternalCall(Call);
      if (Summary.Effect != ExternalCallEffect::Unknown) {
        if (handleExternalCall(*Call, Summary, OutValue, InValue, ContextId)) {
          return OutValue;
        }
      }
    }
    return flowCall(Context, Node, InValue);
  }

  if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Node)) {
    if (isPointerLike(Load)) {
      OutValue.assign(PointsToValue::forValue(Load),
                      loadFromPointer(Load->getPointerOperand(), InValue));
    }
    return OutValue;
  }

  if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Node)) {
    handleStore(*Store, OutValue, InValue);
    return OutValue;
  }

  if (auto *Return = llvm::dyn_cast<llvm::ReturnInst>(Node)) {
    if (auto *RetVal = Return->getReturnValue(); isPointerLike(RetVal)) {
      OutValue.assign(PointsToValue::returnValue(),
                      pointsToSetOfValue(RetVal, InValue));
    } else {
      OutValue.clearReturnValue();
    }
    return OutValue;
  }

  if (isPointerLike(Node)) {
    OutValue.assign(PointsToValue::forValue(Node),
                    evaluateValue(Node, InValue));
  }

  return OutValue;
}

bool PointsToAnalysis::isPointerLike(const llvm::Value *Value) const {
  return Value != nullptr && Value->getType()->isPointerTy();
}

const llvm::DataLayout *PointsToAnalysis::dataLayout() const {
  if (auto *LLVMProgram = llvmProgram()) {
    if (auto *Module = LLVMProgram->getModule()) {
      return &Module->getDataLayout();
    }
  }
  return nullptr;
}

void PointsToAnalysis::seedGlobals(DomainType &State) const {
  auto *LLVMProgram = llvmProgram();
  if (LLVMProgram == nullptr || LLVMProgram->getModule() == nullptr) {
    return;
  }

  const auto &Module = *LLVMProgram->getModule();
  const auto &DL = Module.getDataLayout();
  MemoryModel Model(DL);

  for (auto &Global : Module.globals()) {
    auto *ValueType = Global.getValueType();

    // Treat the global as an addressable object at offset 0 of its allocated
    // type. We always seed the global symbol to point at its own object so
    // later loads through &G evaluate correctly even when the global holds a
    // non-pointer aggregate.
    const auto Object = MemoryBlock::global(&Global, Model.layoutForGlobal(&Global));
    if (Global.getType()->isPointerTy()) {
      State.assign(PointsToValue::forValue(&Global), singletonLocation(Object));
    }

    if (!Global.hasInitializer()) {
      continue;
    }

    // Top-level pointer globals: as before, store the initializer's points-to
    // set in the cell at offset 0.
    if (ValueType->isPointerTy()) {
      State.store(MemoryLocation::exact(Object),
                  initializerTargets(Global.getInitializer()), true);
      continue;
    }

    // Aggregates: walk into ConstantStruct/Array/Vector to seed each pointer
    // field at its layout offset. This recovers precision for global tables of
    // pointers (vtables, function pointer arrays, etc.) that earlier folded
    // into a single summary fact.
    seedGlobalInitializer(&Global, Global.getInitializer(),
                          MemoryLocation::exact(Object), State);
  }
}

void PointsToAnalysis::seedGlobalInitializer(
    const llvm::GlobalVariable *Global, const llvm::Constant *Initializer,
    const MemoryLocation &Base, DomainType &State) const {
  if (Initializer == nullptr || Global == nullptr) {
    return;
  }

  const auto *DL = dataLayout();
  if (DL == nullptr) {
    return;
  }

  if (llvm::isa<llvm::ConstantAggregateZero>(Initializer) ||
      llvm::isa<llvm::UndefValue>(Initializer)) {
    return;
  }

  auto *AggTy = Initializer->getType();
  if (auto *Struct = llvm::dyn_cast<llvm::ConstantStruct>(Initializer)) {
    auto *StructTy = llvm::cast<llvm::StructType>(AggTy);
    if (!StructTy->isSized()) {
      return;
    }
    const auto *SL = DL->getStructLayout(StructTy);
    MemoryModel Model(*DL);
    for (unsigned I = 0, E = Struct->getNumOperands(); I < E; ++I) {
      auto *Element =
          llvm::dyn_cast<llvm::Constant>(Struct->getOperand(I));
      if (Element == nullptr) {
        continue;
      }
      const auto FieldOffset =
          static_cast<std::int64_t>(SL->getElementOffset(I));
      auto Loc = Model.getFieldLocationFromOffset(Base, FieldOffset);
      seedGlobalInitializer(Global, Element,
                            Loc.value_or(MemoryLocation::summary(Base.Object)),
                            State);
    }
    return;
  }

  if (auto *Array = llvm::dyn_cast<llvm::ConstantArray>(Initializer)) {
    auto *ArrTy = llvm::cast<llvm::ArrayType>(AggTy);
    auto *ElemTy = ArrTy->getElementType();
    if (!ElemTy->isSized()) {
      return;
    }
    const auto ElemSize = static_cast<std::int64_t>(DL->getTypeAllocSize(ElemTy));
    MemoryModel Model(*DL);
    for (unsigned I = 0, E = Array->getNumOperands(); I < E; ++I) {
      auto *Element = llvm::dyn_cast<llvm::Constant>(Array->getOperand(I));
      if (Element == nullptr) {
        continue;
      }
      const auto Offset = static_cast<std::int64_t>(I) * ElemSize;
      auto Loc = Model.getFieldLocationFromOffset(Base, Offset);
      seedGlobalInitializer(Global, Element,
                            Loc.value_or(MemoryLocation::summary(Base.Object)),
                            State);
    }
    return;
  }

  if (llvm::isa<llvm::ConstantDataSequential>(Initializer)) {
    // Data sequential constants only contain primitive (non-pointer) elements,
    // so there is nothing to seed.
    return;
  }

  if (Initializer->getType()->isPointerTy()) {
    const auto Targets = initializerTargets(Initializer);
    if (!Targets.empty()) {
      State.store(Base, Targets, true);
    }
    return;
  }

  // Anything else (BlockAddress, plain constants of non-pointer type) does not
  // affect the points-to graph.
}

PointsToObject PointsToAnalysis::classifyObject(const llvm::Value *Value,
                                                std::size_t ContextId) const {
  const auto *DL = dataLayout();
  if (DL == nullptr) {
    return MemoryBlock::summary();
  }
  MemoryModel Model(*DL);

  if (auto *Argument = llvm::dyn_cast<llvm::Argument>(Value)) {
    return MemoryBlock::argument(Argument, Model.layoutForArgument(Argument),
                                 ContextId);
  }
  if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Value)) {
    return MemoryBlock::stack(Alloca, Model.layoutForStack(Alloca), ContextId);
  }
  if (auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value)) {
    return MemoryBlock::global(Global, Model.layoutForGlobal(Global));
  }
  if (auto *Function = llvm::dyn_cast<llvm::Function>(Value)) {
    return MemoryBlock::function(Function);
  }
  if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Value)) {
    return MemoryBlock::heap(Call, Model.layoutForHeapCall(Call), ContextId);
  }
  return MemoryBlock::summary();
}

MemoryLocationSet
PointsToAnalysis::pointsToSetOfValue(const llvm::Value *Value,
                                     const DomainType &State) const {
  return evaluateValue(Value, State);
}

MemoryLocationSet
PointsToAnalysis::evaluateValue(const llvm::Value *Value,
                                const DomainType &State) const {
  if (!isPointerLike(Value)) {
    return {};
  }

  if (llvm::isa<llvm::ConstantPointerNull>(Value)) {
    return {};
  }
  if (auto *Function = llvm::dyn_cast<llvm::Function>(Value)) {
    return singletonLocation(MemoryBlock::function(Function));
  }
  if (auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value)) {
    return singletonLocation(classifyObject(Global));
  }
  if (auto *Alias = llvm::dyn_cast<llvm::GlobalAlias>(Value)) {
    return evaluateValue(Alias->getAliaseeObject(), State);
  }
  if (auto *ConstantExpr = llvm::dyn_cast<llvm::ConstantExpr>(Value)) {
    if (ConstantExpr->isCast()) {
      return evaluateValue(ConstantExpr->getOperand(0), State);
    }
    if (ConstantExpr->getOpcode() == llvm::Instruction::GetElementPtr) {
      const auto Base = evaluateValue(ConstantExpr->getOperand(0), State);
      const auto *DL = dataLayout();
      if (DL == nullptr) {
        return summaryLocation();
      }
      MemoryModel Model(*DL);
      MemoryLocationSet Result;
      for (const auto &Location : Base) {
        auto FieldLocation = Model.getFieldLocation(Location, ConstantExpr);
        Result.insert(
            FieldLocation.value_or(MemoryLocation::summary(Location.Object)));
      }
      return Result;
    }
    return summaryLocation();
  }
  if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Value)) {
    return evaluateValue(Cast->getOperand(0), State);
  }
  if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Value)) {
    const auto Base = evaluateValue(GEP->getPointerOperand(), State);
    const auto *DL = dataLayout();
    if (DL == nullptr) {
      return summaryLocation();
    }
    MemoryModel Model(*DL);
    MemoryLocationSet Result;
    for (const auto &Location : Base) {
      auto FieldLocation = Model.getFieldLocation(Location, GEP);
      Result.insert(
          FieldLocation.value_or(MemoryLocation::summary(Location.Object)));
    }
    return Result;
  }
  if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(Value)) {
    MemoryLocationSet Result;
    for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
      const auto Incoming = pointsToSetOfValue(Phi->getIncomingValue(I), State);
      Result.insert(Incoming.begin(), Incoming.end());
    }
    return Result;
  }
  if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(Value)) {
    auto Result = pointsToSetOfValue(Select->getTrueValue(), State);
    const auto FalseTargets =
        pointsToSetOfValue(Select->getFalseValue(), State);
    Result.insert(FalseTargets.begin(), FalseTargets.end());
    return Result;
  }

  return State.pointsTo(PointsToValue::forValue(Value));
}

MemoryLocationSet
PointsToAnalysis::loadFromPointer(const llvm::Value *Pointer,
                                  const DomainType &State) const {
  MemoryLocationSet Result;
  const auto Locations = pointsToSetOfValue(Pointer, State);
  for (const auto &Location : Locations) {
    const auto &Stored = State.load(Location);
    if (!Stored.empty()) {
      Result.insert(Stored.begin(), Stored.end());
    } else {
      const auto &ObjectSummary = State.load(MemoryLocation::summary(Location.Object));
      if (!ObjectSummary.empty()) {
        Result.insert(ObjectSummary.begin(), ObjectSummary.end());
      } else {
        const auto UnknownTargets = summaryLocation();
        Result.insert(UnknownTargets.begin(), UnknownTargets.end());
      }
    }
  }
  return Result;
}

MemoryLocationSet
PointsToAnalysis::initializerTargets(const llvm::Constant *Initializer) const {
  if (Initializer == nullptr) {
    return summaryLocation();
  }
  if (llvm::isa<llvm::ConstantPointerNull>(Initializer)) {
    return {};
  }
  if (auto *Function = llvm::dyn_cast<llvm::Function>(Initializer)) {
    return singletonLocation(MemoryBlock::function(Function));
  }
  if (auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Initializer)) {
    return singletonLocation(classifyObject(Global));
  }
  if (auto *Alias = llvm::dyn_cast<llvm::GlobalAlias>(Initializer)) {
    return initializerTargets(
        llvm::dyn_cast<llvm::Constant>(Alias->getAliaseeObject()));
  }
  if (auto *ConstantExpr = llvm::dyn_cast<llvm::ConstantExpr>(Initializer)) {
    if (ConstantExpr->isCast() ||
        ConstantExpr->getOpcode() == llvm::Instruction::GetElementPtr) {
      return initializerTargets(
          llvm::dyn_cast<llvm::Constant>(ConstantExpr->getOperand(0)));
    }
  }
  return summaryLocation();
}

void PointsToAnalysis::handleStore(const llvm::StoreInst &Store,
                                   DomainType &OutValue,
                                   const DomainType &InValue) const {
  const auto StoredTargets =
      pointsToSetOfValue(Store.getValueOperand(), InValue);
  const auto Locations = pointsToSetOfValue(Store.getPointerOperand(), InValue);
  if (Locations.empty()) {
    return;
  }

  const bool StrongUpdate = Locations.size() == 1 &&
                            !Locations.begin()->IsSummary &&
                            !Locations.begin()->Object.isSummary();
  for (const auto &Location : Locations) {
    if (Location.IsSummary || Location.Object.isSummary()) {
      OutValue.store(Location, summaryLocation(), false);
      continue;
    }
    OutValue.store(Location, StoredTargets, StrongUpdate);
  }
}

bool PointsToAnalysis::handleMemoryIntrinsicCall(
    const llvm::CallBase &Call, DomainType &OutValue,
    const DomainType &InValue) const {
  const auto *DL = dataLayout();
  if (DL == nullptr || Call.arg_size() < 2) {
    return false;
  }

  MemoryModel Model(*DL);
  if (Model.isMemcpyLikeCall(&Call) || Model.isMemmoveLikeCall(&Call)) {
    copyThroughPointers(Call.getArgOperand(0), Call.getArgOperand(1), OutValue,
                        InValue);
    return true;
  }

  if (Model.isMemsetLikeCall(&Call)) {
    summarizePointerTargets(Call.getArgOperand(0), OutValue, InValue);
    return true;
  }

  return false;
}

bool PointsToAnalysis::handleExternalCall(
    const llvm::CallBase &Call, const ExternalCallSummary &Summary,
    DomainType &OutValue, const DomainType &InValue,
    std::size_t ContextId) const {
  const auto *DL = dataLayout();
  if (DL == nullptr) {
    return false;
  }

  MemoryModel Model(*DL);

  switch (Summary.Effect) {
  case ExternalCallEffect::Noop:
    return true;

  case ExternalCallEffect::Exit:
    // Process exit effects identically to a no-op for pointer state - the CFG
    // structure already prevents successors from running once we hit
    // unreachable / no-return paths.
    return true;

  case ExternalCallEffect::Alloc: {
    if (!Call.getType()->isPointerTy()) {
      return true;
    }
    OutValue.assign(PointsToValue::forValue(&Call),
                    singletonLocation(classifyObject(&Call, ContextId)));
    return true;
  }

  case ExternalCallEffect::StringDup: {
    if (Summary.SrcArgIndex >= Call.arg_size() ||
        !Call.getType()->isPointerTy()) {
      return true;
    }
    // Allocate a fresh heap object for the duplicate.
    const auto NewObject = MemoryBlock::heap(
        &Call, Model.layoutForHeapCall(&Call), ContextId);
    OutValue.assign(PointsToValue::forValue(&Call),
                    singletonLocation(NewObject));

    // Conservatively summarize the bytes so loads from the duplicate yield the
    // same set of summary objects as the source string.
    const auto SrcTargets =
        pointsToSetOfValue(Call.getArgOperand(Summary.SrcArgIndex), InValue);
    const auto DstBase = MemoryLocation::exact(NewObject);
    const bool StrongUpdate = SrcTargets.size() <= 1;
    for (const auto &Src : SrcTargets) {
      copyMemoryObject(DstBase, Src, OutValue, InValue, StrongUpdate);
    }
    return true;
  }

  case ExternalCallEffect::Memcopy: {
    if (Summary.DestArgIndex >= Call.arg_size() ||
        Summary.SrcArgIndex >= Call.arg_size()) {
      return true;
    }
    copyThroughPointers(Call.getArgOperand(Summary.DestArgIndex),
                        Call.getArgOperand(Summary.SrcArgIndex), OutValue,
                        InValue);
    if (Summary.ReturnsDestArg && Call.getType()->isPointerTy()) {
      OutValue.assign(
          PointsToValue::forValue(&Call),
          pointsToSetOfValue(Call.getArgOperand(Summary.DestArgIndex),
                             InValue));
    }
    return true;
  }

  case ExternalCallEffect::Memset: {
    if (Summary.DestArgIndex >= Call.arg_size()) {
      return true;
    }
    summarizePointerTargets(Call.getArgOperand(Summary.DestArgIndex), OutValue,
                            InValue);
    if (Summary.ReturnsDestArg && Call.getType()->isPointerTy()) {
      OutValue.assign(
          PointsToValue::forValue(&Call),
          pointsToSetOfValue(Call.getArgOperand(Summary.DestArgIndex),
                             InValue));
    }
    return true;
  }

  case ExternalCallEffect::ReturnsArgument: {
    if (!Call.getType()->isPointerTy() ||
        Summary.SrcArgIndex >= Call.arg_size()) {
      return true;
    }
    OutValue.assign(
        PointsToValue::forValue(&Call),
        pointsToSetOfValue(Call.getArgOperand(Summary.SrcArgIndex), InValue));
    return true;
  }

  case ExternalCallEffect::AllocatesIntoArgument: {
    if (Summary.DestArgIndex >= Call.arg_size()) {
      return true;
    }
    auto *DestArg = Call.getArgOperand(Summary.DestArgIndex);
    if (!isPointerLike(DestArg)) {
      return true;
    }

    const auto DestLocations = pointsToSetOfValue(DestArg, InValue);
    if (DestLocations.empty()) {
      return true;
    }

    const auto NewObject =
        MemoryBlock::heap(&Call, Model.layoutForHeapCall(&Call), ContextId);
    const auto NewTargets = singletonLocation(NewObject);
    const bool StrongUpdate = DestLocations.size() == 1 &&
                              !DestLocations.begin()->IsSummary &&
                              !DestLocations.begin()->Object.isSummary();
    for (const auto &Location : DestLocations) {
      if (Location.IsSummary || Location.Object.isSummary()) {
        OutValue.store(Location, summaryLocation(), false);
        continue;
      }
      OutValue.store(Location, NewTargets, StrongUpdate);
    }
    return true;
  }

  case ExternalCallEffect::OpaqueReturn: {
    if (!Call.getType()->isPointerTy()) {
      return true;
    }
    // Model the return as a fresh heap allocation (e.g., FILE*, DIR*) so its
    // identity is distinct from any of the arguments.
    OutValue.assign(PointsToValue::forValue(&Call),
                    singletonLocation(classifyObject(&Call, ContextId)));
    return true;
  }

  case ExternalCallEffect::Unknown:
    return false;
  }

  return false;
}

bool PointsToAnalysis::shouldSummarizeUnknownArgument(
    const llvm::CallBase &Call, unsigned ArgIndex) const {
  if (ArgIndex >= Call.arg_size()) {
    return false;
  }

  auto *Arg = Call.getArgOperand(ArgIndex);
  if (!isPointerLike(Arg) || Call.paramHasAttr(ArgIndex, llvm::Attribute::ByVal)) {
    return false;
  }

  if (Call.paramHasAttr(ArgIndex, llvm::Attribute::ReadOnly) ||
      Call.paramHasAttr(ArgIndex, llvm::Attribute::ReadNone)) {
    return false;
  }

  if (Call.onlyReadsMemory() || Call.doesNotAccessMemory()) {
    return false;
  }

  return true;
}

void PointsToAnalysis::summarizeUnknownCallEffects(
    const llvm::CallBase &Call, DomainType &OutValue,
    const DomainType &InValue, std::size_t ContextId) const {
  if (Call.getType()->isPointerTy()) {
    bool ModeledReturn = false;
    for (unsigned I = 0; I < Call.arg_size(); ++I) {
      if (!Call.paramHasAttr(I, llvm::Attribute::Returned)) {
        continue;
      }
      OutValue.assign(PointsToValue::forValue(&Call),
                      pointsToSetOfValue(Call.getArgOperand(I), InValue));
      ModeledReturn = true;
      break;
    }

    if (!ModeledReturn) {
      const auto *Callee = Call.getCalledFunction();
      const bool FreshReturn =
          Call.hasRetAttr(llvm::Attribute::NoAlias) ||
          Call.hasFnAttr(llvm::Attribute::NoAlias) ||
          (Callee != nullptr &&
           Callee->hasFnAttribute(llvm::Attribute::AllocSize));
      if (FreshReturn) {
        OutValue.assign(PointsToValue::forValue(&Call),
                        singletonLocation(classifyObject(&Call, ContextId)));
      } else {
        OutValue.assign(PointsToValue::forValue(&Call), summaryLocation());
      }
    }
  }

  for (unsigned I = 0; I < Call.arg_size(); ++I) {
    if (!shouldSummarizeUnknownArgument(Call, I)) {
      continue;
    }
    const auto Targets = pointsToSetOfValue(Call.getArgOperand(I), OutValue);
    for (const auto &Target : Targets) {
      OutValue.summarizeObject(Target.Object);
    }
  }
}

void PointsToAnalysis::copyThroughPointers(const llvm::Value *Dst,
                                           const llvm::Value *Src,
                                           DomainType &OutValue,
                                           const DomainType &InValue) const {
  if (!isPointerLike(Dst) || !isPointerLike(Src)) {
    // Conservatively summarize the destination if either side is not a
    // pointer-typed value (e.g. memset takes an integer pattern).
    if (isPointerLike(Dst)) {
      summarizePointerTargets(Dst, OutValue, InValue);
    }
    return;
  }

  const auto DstTargets = pointsToSetOfValue(Dst, InValue);
  const auto SrcTargets = pointsToSetOfValue(Src, InValue);
  if (DstTargets.empty() || SrcTargets.empty()) {
    return;
  }
  const bool StrongUpdate = DstTargets.size() == 1 && SrcTargets.size() == 1 &&
                            !DstTargets.begin()->IsSummary &&
                            !DstTargets.begin()->Object.isSummary() &&
                            !SrcTargets.begin()->IsSummary &&
                            !SrcTargets.begin()->Object.isSummary();
  for (const auto &DstLoc : DstTargets) {
    for (const auto &SrcLoc : SrcTargets) {
      copyMemoryObject(DstLoc, SrcLoc, OutValue, InValue, StrongUpdate);
    }
  }
}

bool PointsToAnalysis::handleReallocCall(const llvm::CallBase &Call,
                                         DomainType &OutValue,
                                         const DomainType &InValue,
                                         std::size_t ContextId) const {
  const auto *DL = dataLayout();
  if (DL == nullptr) {
    return false;
  }

  MemoryModel Model(*DL);
  if (!Model.isReallocLikeAllocator(&Call) || Call.arg_empty() ||
      !Call.getType()->isPointerTy()) {
    return false;
  }

  const auto OldTargets = pointsToSetOfValue(Call.getArgOperand(0), InValue);
  MemoryLocationSet ReturnTargets;

  for (const auto &Old : OldTargets) {
    const auto UpdatedLayout = Model.layoutForReallocCall(&Call, Old.Object);
    const auto NewBlock = MemoryBlock::heap(&Call, UpdatedLayout, ContextId);
    copyMemoryObject(MemoryLocation::exact(NewBlock), Old, OutValue, InValue,
                     OldTargets.size() <= 1 && !Old.IsSummary &&
                         !Old.Object.isSummary());
    ReturnTargets.insert(MemoryLocation::exact(NewBlock));
  }

  if (ReturnTargets.empty()) {
    ReturnTargets.insert(MemoryLocation::exact(
        MemoryBlock::heap(&Call, Model.layoutForHeapCall(&Call), ContextId)));
  }
  OutValue.assign(PointsToValue::forValue(&Call), ReturnTargets);

  summarizePointerTargets(Call.getArgOperand(0), OutValue, InValue);
  return true;
}

void PointsToAnalysis::copyMemoryObject(const MemoryLocation &DstBase,
                                        const MemoryLocation &SrcBase,
                                        DomainType &OutValue,
                                        const DomainType &InValue,
                                        bool StrongUpdate) const {
  if (DstBase.IsSummary || DstBase.Object.isSummary() || SrcBase.IsSummary ||
      SrcBase.Object.isSummary()) {
    OutValue.summarizeObject(DstBase.Object);
    return;
  }

  bool CopiedAny = false;
  for (const auto &Entry : InValue.getMemory()) {
    if (!(Entry.first.Object == SrcBase.Object) ||
        Entry.first.Offset < SrcBase.Offset) {
      continue;
    }
    auto DstLocation = Entry.first;
    DstLocation.Object = DstBase.Object;
    DstLocation.Offset = DstBase.Offset + (Entry.first.Offset - SrcBase.Offset);
    OutValue.store(DstLocation, Entry.second, StrongUpdate);
    CopiedAny = true;
  }

  const auto &SrcSummary = InValue.load(MemoryLocation::summary(SrcBase.Object));
  if (!SrcSummary.empty()) {
    OutValue.store(MemoryLocation::summary(DstBase.Object), SrcSummary, false);
    CopiedAny = true;
  }

  if (!CopiedAny) {
    OutValue.summarizeObject(DstBase.Object);
  }
}

void PointsToAnalysis::summarizePointerTargets(
    const llvm::Value *Pointer, DomainType &OutValue,
    const DomainType &InValue) const {
  if (!isPointerLike(Pointer)) {
    return;
  }

  const auto Targets = pointsToSetOfValue(Pointer, InValue);
  for (const auto &Target : Targets) {
    OutValue.summarizeObject(Target.Object);
  }
}

PointsToAnalysis::ResolverReturn
PointsToAnalysis::resolveIndirectTargets(const llvm::CallBase *Call,
                                         const DomainType &State) const {
  if (Call == nullptr || !Call->isIndirectCall()) {
    return std::vector<MethodType>{};
  }

  std::vector<MethodType> Targets;
  const auto CalleeTargets =
      pointsToSetOfValue(Call->getCalledOperand(), State);
  for (const auto &Target : CalleeTargets) {
    if (Target.Object.kind() == AllocationSiteKind::Function) {
      auto *Function = llvm::dyn_cast<llvm::Function>(
          const_cast<llvm::Value *>(Target.Object.value()));
      if (Function != nullptr) {
        Targets.push_back(Function);
      }
    } else if (Target.IsSummary || Target.Object.isSummary()) {
      return std::nullopt;
    }
  }

  if (Targets.empty()) {
    return std::nullopt;
  }
  return Targets;
}

std::optional<PointsToAnalysis::DomainType>
PointsToAnalysis::processKnownCall(ContextPtr CallerContext, NodeType Node,
                                   const MethodType &Method,
                                   const DomainType &EntryValue) {
  if (Program.isPhantomMethod(Method)) {
    DomainType ExitValue = EntryValue;
    if (Method->getReturnType()->isPointerTy()) {
      ExitValue.assign(PointsToValue::returnValue(), summaryLocation());
    } else {
      ExitValue.clearReturnValue();
    }
    return ExitValue;
  }
  return this->processCall(CallerContext, Node, Method, EntryValue);
}

std::optional<PointsToAnalysis::DomainType> PointsToAnalysis::processCallTarget(
    ContextPtr CallerContext, NodeType Call, const MethodType &Method,
    const DomainType &InValue, DomainType &Accumulated) {
  if (Call == nullptr || Method == nullptr) {
    return std::nullopt;
  }

  auto *CallBase = llvm::dyn_cast<llvm::CallBase>(Call);
  const auto *DL = dataLayout();
  if (CallBase == nullptr || DL == nullptr) {
    return std::nullopt;
  }

  const auto ContextId = CallerContext != nullptr ? CallerContext->getId() : 0;

  // Phantom (declaration-only) callees go through the external library model
  // so well-known libc functions get precise summaries instead of a blanket
  // argument escape. This preserves call-graph transitions for callers that
  // need a concrete callee identity.
  if (Program.isPhantomMethod(Method)) {
    MemoryModel Model(*DL);
    const auto Summary = Model.classifyExternalCall(CallBase);
    if (Summary.Effect != ExternalCallEffect::Unknown) {
      DomainType Returned = InValue;
      const bool Handled =
          handleExternalCall(*CallBase, Summary, Returned, InValue, ContextId);
      if (Handled) {
        Accumulated = this->meet(Accumulated, Returned);
        return Returned;
      }
    }
  }

  DomainType EntryValue;
  for (unsigned I = 0; I < CallBase->arg_size() && I < Method->arg_size();
       ++I) {
    auto *ArgIt = Method->arg_begin();
    std::advance(ArgIt, I);
    if (!isPointerLike(&*ArgIt)) {
      continue;
    }
    EntryValue.assign(PointsToValue::forValue(&*ArgIt),
                      pointsToSetOfValue(CallBase->getArgOperand(I), InValue));
  }
  seedGlobals(EntryValue);

  auto ExitValue = processKnownCall(CallerContext, Call, Method, EntryValue);
  if (!ExitValue.has_value()) {
    return std::nullopt;
  }

  DomainType Returned = InValue;
  if (CallBase->getType()->isPointerTy()) {
    Returned.assign(PointsToValue::forValue(Call),
                    ExitValue->pointsTo(PointsToValue::returnValue()));
  }

  for (const auto &Entry : ExitValue->getRoots()) {
    if (!Entry.first.IsReturnValue && Entry.first.Value != nullptr &&
        llvm::isa<llvm::GlobalVariable>(Entry.first.Value)) {
      Returned.assign(Entry.first, Entry.second);
    }
  }
  Returned.unionMemoryFrom(*ExitValue);

  if (Program.isPhantomMethod(Method)) {
    summarizeUnknownCallEffects(*CallBase, Returned, Returned, ContextId);
  }

  Accumulated = this->meet(Accumulated, Returned);
  return Returned;
}

std::optional<PointsToAnalysis::DomainType>
PointsToAnalysis::flowCall(ContextPtr Context, NodeType Call,
                           const DomainType &InValue) {
  auto *CallBase = llvm::dyn_cast<llvm::CallBase>(Call);
  if (CallBase == nullptr) {
    return std::nullopt;
  }

  auto Targets = Program.resolveTargets(Context->getMethod(), Call);
  if ((!Targets.has_value() || Targets->empty()) &&
      CallBase->isIndirectCall()) {
    Targets = resolveIndirectTargets(CallBase, InValue);
  }

  if (!Targets.has_value()) {
    this->ContextTransitions.addTransition(
        typename Base::CallSiteType(Context, Call), nullptr);
    DomainType Unknown = InValue;
    summarizeUnknownCallEffects(*CallBase, Unknown, InValue,
                                Context->getId());
    return Unknown;
  }

  if (Targets->empty()) {
    return InValue;
  }

  DomainType Result = topValue();
  bool AnyResolved = false;
  for (auto *Target : *Targets) {
    if (!Target) {
      continue;
    }
    auto Returned = processCallTarget(Context, Call, Target, InValue, Result);
    if (Returned.has_value()) {
      AnyResolved = true;
    }
  }

  if (!AnyResolved) {
    return std::nullopt;
  }
  return Result;
}

const DefaultLLVMProgramRepresentation *PointsToAnalysis::llvmProgram() const {
  return dynamic_cast<const DefaultLLVMProgramRepresentation *>(&Program);
}

// ---------------------------------------------------------------------------
// Convenience query API
// ---------------------------------------------------------------------------

MemoryLocationSet
PointsToAnalysis::getPointsToSet(const llvm::Value *Value) const {
  if (!isPointerLike(Value)) {
    return {};
  }

  // Aggregate the points-to facts attached to `Value` over every context that
  // reached the enclosing method. For globals/functions we still go through
  // evaluateValue so they are resolved even before any analysis ran.
  if (auto *Inst = llvm::dyn_cast<llvm::Instruction>(Value)) {
    auto *F = Inst->getFunction();
    if (F == nullptr) {
      return {};
    }
    MemoryLocationSet Result;
    for (const auto &Context : this->getContexts(const_cast<llvm::Function *>(F))) {
      if (Context->isFreed()) {
        continue;
      }
      const auto Found =
          Context->getValueAfter(const_cast<llvm::Instruction *>(Inst)).pointsTo(PointsToValue::forValue(Inst));
      Result.insert(Found.begin(), Found.end());
    }
    if (!Result.empty()) {
      return Result;
    }
  }

  if (auto *Arg = llvm::dyn_cast<llvm::Argument>(Value)) {
    auto *F = Arg->getParent();
    if (F == nullptr) {
      return {};
    }
    MemoryLocationSet Result;
    for (const auto &Context : this->getContexts(const_cast<llvm::Function *>(F))) {
      if (Context->isFreed()) {
        continue;
      }
      const auto Found =
          Context->getEntryValue().pointsTo(PointsToValue::forValue(Arg));
      Result.insert(Found.begin(), Found.end());
    }
    return Result;
  }

  // Fall back to a state-free evaluation for globals, functions, and other
  // top-level constants whose points-to set is independent of program state.
  PointsToGraph EmptyState;
  return evaluateValue(Value, EmptyState);
}

MemoryLocationSet
PointsToAnalysis::getPointsToSetBefore(const llvm::Value *Value,
                                       const llvm::Instruction *AtNode) const {
  if (!isPointerLike(Value) || AtNode == nullptr) {
    return {};
  }

  auto *F = AtNode->getFunction();
  if (F == nullptr) {
    return {};
  }

  MemoryLocationSet Result;
  for (const auto &Context : this->getContexts(const_cast<llvm::Function *>(F))) {
    if (Context->isFreed()) {
      continue;
    }
    const auto &State =
        Context->getValueBefore(const_cast<llvm::Instruction *>(AtNode));
    const auto Found = evaluateValue(Value, State);
    Result.insert(Found.begin(), Found.end());
  }
  return Result;
}

MemoryLocationSet
PointsToAnalysis::getPointsToSetAfter(const llvm::Value *Value,
                                      const llvm::Instruction *AtNode) const {
  if (!isPointerLike(Value) || AtNode == nullptr) {
    return {};
  }

  auto *F = AtNode->getFunction();
  if (F == nullptr) {
    return {};
  }

  MemoryLocationSet Result;
  for (const auto &Context : this->getContexts(const_cast<llvm::Function *>(F))) {
    if (Context->isFreed()) {
      continue;
    }
    const auto &State =
        Context->getValueAfter(const_cast<llvm::Instruction *>(AtNode));
    const auto Found = evaluateValue(Value, State);
    Result.insert(Found.begin(), Found.end());
  }
  return Result;
}

std::set<llvm::Function *>
PointsToAnalysis::getCallTargets(const llvm::CallBase *Call) const {
  std::set<llvm::Function *> Targets;
  if (Call == nullptr) {
    return Targets;
  }

  if (auto *Direct = Call->getCalledFunction()) {
    Targets.insert(Direct);
  }

  // Walk the context-transition table to recover all callees seen across
  // every context that reached this call site.
  for (const auto &Entry : this->getContextTransitionTable().getTransitions()) {
    if (Entry.first.getCallNode() != Call) {
      continue;
    }
    for (const auto &Target : Entry.second) {
      if (Target.first != nullptr) {
        Targets.insert(Target.first);
      }
    }
  }

  return Targets;
}

bool PointsToAnalysis::isDefaultCallSite(const llvm::CallBase *Call) const {
  if (Call == nullptr) {
    return false;
  }
  for (const auto &Site :
       this->getContextTransitionTable().getDefaultCallSites()) {
    if (Site.getCallNode() == Call) {
      return true;
    }
  }
  return false;
}

bool PointsToAnalysis::mayAlias(const llvm::Value *V1,
                                const llvm::Value *V2) const {
  if (!isPointerLike(V1) || !isPointerLike(V2)) {
    return false;
  }
  if (V1 == V2) {
    return true;
  }
  const auto Set1 = getPointsToSet(V1);
  const auto Set2 = getPointsToSet(V2);
  for (const auto &Loc1 : Set1) {
    if (Loc1.Object.isSummary()) {
      return true;
    }
    for (const auto &Loc2 : Set2) {
      if (Loc2.Object.isSummary()) {
        return true;
      }
      if (Loc1.Object == Loc2.Object) {
        return true;
      }
    }
  }
  return false;
}

DefaultLLVMProgramRepresentation::ResolveTargetsFn
PointsToAnalysis::callTargetResolver() const {
  // Capture by value of `this` is safe because the analysis object outlives
  // the resolver lambda for the duration of any client analysis - the lambda
  // is only valid as long as `*this` is live.
  const auto *Self = this;
  return [Self](llvm::Function *,
                llvm::Instruction *Node) -> std::optional<std::vector<llvm::Function *>> {
    auto *Call = llvm::dyn_cast<llvm::CallBase>(Node);
    if (Call == nullptr) {
      return std::vector<llvm::Function *>{};
    }

    if (auto *Direct = Call->getCalledFunction()) {
      if (Direct->isDeclaration()) {
        return std::nullopt;
      }
      return std::vector<llvm::Function *>{Direct};
    }

    if (Self->isDefaultCallSite(Call)) {
      return std::nullopt;
    }

    auto Found = Self->getCallTargets(Call);
    if (Found.empty()) {
      return std::nullopt;
    }
    return std::vector<llvm::Function *>(Found.begin(), Found.end());
  };
}

} // namespace llvmir
} // namespace vasco
