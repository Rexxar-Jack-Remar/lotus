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
      if (handleMemoryIntrinsicCall(*Call, OutValue, InValue)) {
        return OutValue;
      }
      if (handleReallocCall(*Call, OutValue, InValue, ContextId)) {
        return OutValue;
      }
      if (Model.isHeapAllocator(Call)) {
        OutValue.assign(PointsToValue::forValue(Call),
                        singletonLocation(classifyObject(Call, ContextId)));
        return OutValue;
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

  MemoryModel Model(LLVMProgram->getModule()->getDataLayout());
  for (auto &Global : LLVMProgram->getModule()->globals()) {
    if (!Global.getValueType()->isPointerTy()) {
      continue;
    }

    const auto Object =
        MemoryBlock::global(&Global, Model.layoutForGlobal(&Global));
    State.assign(PointsToValue::forValue(&Global), singletonLocation(Object));
    State.store(MemoryLocation::exact(Object),
                initializerTargets(Global.getInitializer()), true);
  }
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
      Result.insert(MemoryLocation::summary(Location.Object, Location.Offset));
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
    const auto DstTargets = pointsToSetOfValue(Call.getArgOperand(0), InValue);
    const auto SrcTargets = pointsToSetOfValue(Call.getArgOperand(1), InValue);
    for (const auto &Dst : DstTargets) {
      for (const auto &Src : SrcTargets) {
        copyMemoryObject(Dst.Object, Src.Object, OutValue, InValue);
      }
    }
    return true;
  }

  if (Model.isMemsetLikeCall(&Call)) {
    summarizePointerTargets(Call.getArgOperand(0), OutValue, InValue);
    return true;
  }

  return false;
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
  const auto NewObject =
      MemoryBlock::heap(&Call, Model.layoutForHeapCall(&Call), ContextId);
  OutValue.assign(PointsToValue::forValue(&Call), singletonLocation(NewObject));

  for (const auto &Old : OldTargets) {
    const auto UpdatedLayout = Model.layoutForReallocCall(&Call, Old.Object);
    const auto NewBlock = MemoryBlock::heap(&Call, UpdatedLayout, ContextId);
    copyMemoryObject(NewBlock, Old.Object, OutValue, InValue);
    OutValue.assign(PointsToValue::forValue(&Call),
                    singletonLocation(NewBlock));
  }

  summarizePointerTargets(Call.getArgOperand(0), OutValue, InValue);
  return true;
}

void PointsToAnalysis::copyMemoryObject(const MemoryBlock &DstObject,
                                        const MemoryBlock &SrcObject,
                                        DomainType &OutValue,
                                        const DomainType &InValue) const {
  for (const auto &Entry : InValue.getMemory()) {
    if (!(Entry.first.Object == SrcObject)) {
      continue;
    }
    auto DstLocation = Entry.first;
    DstLocation.Object = DstObject;
    OutValue.store(DstLocation, Entry.second, true);
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

  MemoryModel Model(*DL);
  const auto ContextId = CallerContext != nullptr ? CallerContext->getId() : 0;

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
    for (unsigned I = 0; I < CallBase->arg_size(); ++I) {
      if (!isPointerLike(CallBase->getArgOperand(I))) {
        continue;
      }
      const auto Targets =
          pointsToSetOfValue(CallBase->getArgOperand(I), Returned);
      for (const auto &Target : Targets) {
        Returned.summarizeObject(Target.Object);
      }
    }
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
    if (CallBase->getType()->isPointerTy()) {
      Unknown.assign(PointsToValue::forValue(Call), summaryLocation());
    }
    for (unsigned I = 0; I < CallBase->arg_size(); ++I) {
      if (!isPointerLike(CallBase->getArgOperand(I))) {
        continue;
      }
      const auto TargetsForArg =
          pointsToSetOfValue(CallBase->getArgOperand(I), Unknown);
      for (const auto &Target : TargetsForArg) {
        Unknown.summarizeObject(Target.Object);
      }
    }
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

} // namespace llvmir
} // namespace vasco
