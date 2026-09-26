#include "Analysis/CFG/ExternCallbackModel.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>

namespace lotus {

namespace {

enum class CallbackTypeKind { Metadata, PthreadCreate, OpenMP };

struct CallbackSpec {
  int CalleeArg = -1;
  llvm::SmallVector<int, 4> CallbackArgs;
  bool PassVarArgs = false;
  CallbackTypeKind TypeKind = CallbackTypeKind::Metadata;
};

using CallbackSpecs = llvm::SmallVector<CallbackSpec, 1>;

struct RewriteCandidate {
  llvm::CallBase *Call = nullptr;
  llvm::Function *Broker = nullptr;
  CallbackSpecs Specs;
  size_t Id = 0;
};

std::optional<int64_t> getIntFromMetadata(const llvm::Metadata *MD) {
  const auto *ConstMD = llvm::dyn_cast_or_null<llvm::ConstantAsMetadata>(MD);
  if (!ConstMD) {
    return std::nullopt;
  }

  const auto *ConstInt = llvm::dyn_cast<llvm::ConstantInt>(ConstMD->getValue());
  if (!ConstInt) {
    return std::nullopt;
  }

  return ConstInt->getSExtValue();
}

std::optional<bool> getBoolFromMetadata(const llvm::Metadata *MD) {
  auto Int = getIntFromMetadata(MD);
  if (!Int) {
    return std::nullopt;
  }
  return *Int != 0;
}

std::optional<CallbackSpec> parseCallbackTuple(const llvm::MDNode &Tuple) {
  if (Tuple.getNumOperands() < 2) {
    return std::nullopt;
  }

  auto CalleeArg = getIntFromMetadata(Tuple.getOperand(0).get());
  auto PassVarArgs =
      getBoolFromMetadata(Tuple.getOperand(Tuple.getNumOperands() - 1).get());
  if (!CalleeArg || !PassVarArgs) {
    return std::nullopt;
  }

  CallbackSpec Spec;
  Spec.CalleeArg = static_cast<int>(*CalleeArg);
  Spec.PassVarArgs = *PassVarArgs;

  for (unsigned Idx = 1; Idx + 1 < Tuple.getNumOperands(); ++Idx) {
    auto CallbackArg = getIntFromMetadata(Tuple.getOperand(Idx).get());
    if (!CallbackArg) {
      return std::nullopt;
    }
    Spec.CallbackArgs.push_back(static_cast<int>(*CallbackArg));
  }

  return Spec;
}

CallbackSpecs parseCallbackMetadata(const llvm::Function &Broker) {
  CallbackSpecs Specs;
  const auto *CallbackMD = Broker.getMetadata("callback");
  if (!CallbackMD) {
    return Specs;
  }

  for (const auto &Op : CallbackMD->operands()) {
    const auto *Tuple = llvm::dyn_cast_or_null<llvm::MDNode>(Op.get());
    if (!Tuple) {
      continue;
    }

    if (auto Spec = parseCallbackTuple(*Tuple)) {
      Specs.push_back(std::move(*Spec));
    }
  }

  return Specs;
}

CallbackSpecs getKnownCallbackSpecs(llvm::StringRef BrokerName) {
  CallbackSpecs Specs;

  auto Known = llvm::StringSwitch<int>(BrokerName)
                   .Case("pthread_create", 0)
                   .Case("__kmpc_fork_call", 1)
                   .Case("__kmpc_fork_teams", 1)
                   .Default(-1);

  switch (Known) {
  case 0:
    Specs.push_back(
        CallbackSpec{2, {3}, false, CallbackTypeKind::PthreadCreate});
    break;
  case 1:
    Specs.push_back(CallbackSpec{2, {-1, -1}, true, CallbackTypeKind::OpenMP});
    break;
  default:
    break;
  }

  return Specs;
}

CallbackSpecs getCallbackSpecs(const llvm::Function &Broker) {
  auto Specs = parseCallbackMetadata(Broker);
  if (!Specs.empty()) {
    return Specs;
  }

  return getKnownCallbackSpecs(Broker.getName());
}

const llvm::Function *getDirectFunction(const llvm::Value *V) {
  if (!V) {
    return nullptr;
  }
  return llvm::dyn_cast<llvm::Function>(V->stripPointerCastsAndAliases());
}

llvm::Type *getPointerType(llvm::LLVMContext &Ctx) {
  return llvm::Type::getInt8PtrTy(Ctx);
}

llvm::Type *getCallbackArgType(const llvm::CallBase &BrokerCall, int ArgNo) {
  if (ArgNo < 0) {
    return getPointerType(BrokerCall.getContext());
  }

  if (static_cast<unsigned>(ArgNo) >= BrokerCall.arg_size()) {
    return nullptr;
  }

  return BrokerCall.getArgOperand(static_cast<unsigned>(ArgNo))->getType();
}

bool appendCallbackParamTypes(llvm::SmallVectorImpl<llvm::Type *> &ParamTys,
                              const llvm::CallBase &BrokerCall,
                              const CallbackSpec &Spec) {
  for (int ArgNo : Spec.CallbackArgs) {
    auto *Ty = getCallbackArgType(BrokerCall, ArgNo);
    if (!Ty) {
      return false;
    }
    ParamTys.push_back(Ty);
  }

  if (Spec.PassVarArgs) {
    unsigned FirstVarArg = BrokerCall.getFunctionType()->getNumParams();
    for (unsigned ArgNo = FirstVarArg; ArgNo < BrokerCall.arg_size(); ++ArgNo) {
      ParamTys.push_back(BrokerCall.getArgOperand(ArgNo)->getType());
    }
  }

  return true;
}

llvm::FunctionType *inferFallbackCallbackType(const llvm::Function &Broker,
                                              const llvm::CallBase &BrokerCall,
                                              const CallbackSpec &Spec) {
  switch (Spec.TypeKind) {
  case CallbackTypeKind::PthreadCreate:
    return llvm::FunctionType::get(getPointerType(Broker.getContext()),
                                   {getPointerType(Broker.getContext())},
                                   false);
  case CallbackTypeKind::OpenMP: {
    llvm::SmallVector<llvm::Type *, 8> ParamTys;
    if (!appendCallbackParamTypes(ParamTys, BrokerCall, Spec)) {
      return nullptr;
    }
    return llvm::FunctionType::get(llvm::Type::getVoidTy(Broker.getContext()),
                                   ParamTys, false);
  }
  case CallbackTypeKind::Metadata: {
    llvm::SmallVector<llvm::Type *, 8> ParamTys;
    if (!appendCallbackParamTypes(ParamTys, BrokerCall, Spec)) {
      return nullptr;
    }
    return llvm::FunctionType::get(llvm::Type::getVoidTy(Broker.getContext()),
                                   ParamTys, false);
  }
  }

  return nullptr;
}

llvm::FunctionType *inferCallbackType(const llvm::Function &Broker,
                                      const llvm::CallBase &BrokerCall,
                                      const CallbackSpec &Spec) {
  if (Spec.CalleeArg < 0 ||
      static_cast<unsigned>(Spec.CalleeArg) >= BrokerCall.arg_size()) {
    return nullptr;
  }

  const auto *CallbackFn =
      getDirectFunction(BrokerCall.getArgOperand(Spec.CalleeArg));
  if (!CallbackFn) {
    return inferFallbackCallbackType(Broker, BrokerCall, Spec);
  }

  return CallbackFn->getFunctionType();
}

bool hasBrokerArg(const llvm::CallBase &BrokerCall, int ArgNo) noexcept {
  return ArgNo >= 0 && static_cast<unsigned>(ArgNo) < BrokerCall.arg_size();
}

unsigned getCallbackArgCount(const llvm::CallBase &BrokerCall,
                             const CallbackSpec &Spec) {
  unsigned Count = Spec.CallbackArgs.size();
  if (!Spec.PassVarArgs) {
    return Count;
  }

  unsigned FirstVarArg = BrokerCall.getFunctionType()->getNumParams();
  if (FirstVarArg >= BrokerCall.arg_size()) {
    return Count;
  }
  return Count + BrokerCall.arg_size() - FirstVarArg;
}

bool hasExpectedCallbackArgCount(const llvm::FunctionType &CBTy,
                                 unsigned ArgCount) noexcept {
  if (CBTy.isVarArg()) {
    return ArgCount >= CBTy.getNumParams();
  }
  return ArgCount == CBTy.getNumParams();
}

bool validateSpec(const llvm::Function &Broker,
                  const llvm::CallBase &BrokerCall, const CallbackSpec &Spec) {
  if (!hasBrokerArg(BrokerCall, Spec.CalleeArg)) {
    return false;
  }

  auto CalleeArg = static_cast<unsigned>(Spec.CalleeArg);
  if (!BrokerCall.getArgOperand(CalleeArg)->getType()->isPointerTy()) {
    return false;
  }

  for (int ArgNo : Spec.CallbackArgs) {
    if (ArgNo >= 0 && !hasBrokerArg(BrokerCall, ArgNo)) {
      return false;
    }
  }

  auto *CBTy = inferCallbackType(Broker, BrokerCall, Spec);
  if (!CBTy) {
    return false;
  }

  unsigned ArgCount = getCallbackArgCount(BrokerCall, Spec);
  return hasExpectedCallbackArgCount(*CBTy, ArgCount);
}

bool validateSpecs(const llvm::Function &Broker,
                   const llvm::CallBase &BrokerCall,
                   llvm::ArrayRef<CallbackSpec> Specs) {
  for (const auto &Spec : Specs) {
    if (!validateSpec(Broker, BrokerCall, Spec)) {
      return false;
    }
  }
  return true;
}

llvm::Value *adaptValue(llvm::IRBuilder<> &IRB, llvm::Value *Value,
                        llvm::Type *ExpectedTy) {
  if (Value->getType() == ExpectedTy) {
    return Value;
  }

  if (Value->getType()->isPointerTy() && ExpectedTy->isPointerTy()) {
    return IRB.CreateBitOrPointerCast(Value, ExpectedTy);
  }

  if (Value->getType()->isIntegerTy() && ExpectedTy->isIntegerTy()) {
    return IRB.CreateIntCast(Value, ExpectedTy, false);
  }

  if (Value->getType()->isPointerTy() && ExpectedTy->isIntegerTy()) {
    return IRB.CreatePtrToInt(Value, ExpectedTy);
  }

  if (Value->getType()->isIntegerTy() && ExpectedTy->isPointerTy()) {
    return IRB.CreateIntToPtr(Value, ExpectedTy);
  }

  return llvm::UndefValue::get(ExpectedTy);
}

llvm::SmallVector<llvm::Value *, 4>
collectCallbackArgs(llvm::IRBuilder<> &IRB, llvm::Function &Model,
                    const llvm::CallBase &BrokerCall, llvm::FunctionType &CBTy,
                    const CallbackSpec &Spec) {
  llvm::SmallVector<llvm::Value *, 4> Args;

  auto GetBrokerArg = [&Model](unsigned ArgNo) -> llvm::Argument * {
    auto *It = Model.arg_begin();
    std::advance(It, ArgNo);
    return &*It;
  };

  for (int ArgNo : Spec.CallbackArgs) {
    if (ArgNo < 0 || static_cast<unsigned>(ArgNo) >= BrokerCall.arg_size()) {
      Args.push_back(nullptr);
      continue;
    }

    Args.push_back(GetBrokerArg(static_cast<unsigned>(ArgNo)));
  }

  if (Spec.PassVarArgs) {
    unsigned FirstVarArg = BrokerCall.getFunctionType()->getNumParams();
    for (unsigned ArgNo = FirstVarArg; ArgNo < BrokerCall.arg_size(); ++ArgNo) {
      Args.push_back(GetBrokerArg(ArgNo));
    }
  }

  for (unsigned Idx = 0,
                End = std::min<unsigned>(Args.size(), CBTy.getNumParams());
       Idx < End; ++Idx) {
    auto *ExpectedTy = CBTy.getParamType(Idx);
    if (!Args[Idx]) {
      Args[Idx] = llvm::UndefValue::get(ExpectedTy);
      continue;
    }

    Args[Idx] = adaptValue(IRB, Args[Idx], ExpectedTy);
  }

  return Args;
}

llvm::FunctionType *createModelType(llvm::CallBase &BrokerCall) {
  llvm::SmallVector<llvm::Type *, 8> ParamTys;
  ParamTys.reserve(BrokerCall.arg_size());
  for (auto &Arg : BrokerCall.args()) {
    ParamTys.push_back(Arg->getType());
  }

  return llvm::FunctionType::get(BrokerCall.getType(), ParamTys, false);
}

std::string createModelName(const llvm::Function &Broker, size_t Id) {
  std::string Name;
  llvm::raw_string_ostream OS(Name);
  OS << ExternCallbackModel::ModelPrefix << '.' << Broker.getName() << '.'
     << Id;
  return OS.str();
}

llvm::Value *createOriginalBrokerCall(llvm::IRBuilder<> &IRB,
                                      llvm::Function &Model,
                                      llvm::CallBase &BrokerCall) {
  llvm::SmallVector<llvm::Value *, 8> Args;
  Args.reserve(BrokerCall.arg_size());
  for (auto &Arg : Model.args()) {
    Args.push_back(&Arg);
  }

  llvm::SmallVector<llvm::OperandBundleDef, 2> OperandBundles;
  BrokerCall.getOperandBundlesAsDefs(OperandBundles);

  auto *Call =
      IRB.CreateCall(BrokerCall.getFunctionType(),
                     BrokerCall.getCalledOperand(), Args, OperandBundles);
  Call->setCallingConv(BrokerCall.getCallingConv());
  Call->setAttributes(BrokerCall.getAttributes());
  Call->setDebugLoc(BrokerCall.getDebugLoc());
  Call->copyMetadata(BrokerCall);
  return Call;
}

void createCallbackCall(llvm::IRBuilder<> &IRB, llvm::Function &Model,
                        const llvm::Function &Broker,
                        llvm::CallBase &BrokerCall, const CallbackSpec &Spec) {
  auto *CBTy = inferCallbackType(Broker, BrokerCall, Spec);
  if (!CBTy) {
    return;
  }

  auto CalleeArg = static_cast<unsigned>(Spec.CalleeArg);
  llvm::Value *CallbackCallee = Model.getArg(CalleeArg);
  if (const auto *DirectCallback =
          getDirectFunction(BrokerCall.getArgOperand(CalleeArg))) {
    CallbackCallee = const_cast<llvm::Function *>(DirectCallback);
  }
  if (CallbackCallee->getType() != CBTy->getPointerTo()) {
    CallbackCallee =
        IRB.CreateBitOrPointerCast(CallbackCallee, CBTy->getPointerTo());
  }
  auto Args = collectCallbackArgs(IRB, Model, BrokerCall, *CBTy, Spec);
  auto *Call = IRB.CreateCall(CBTy, CallbackCallee, Args);
  Call->setDebugLoc(BrokerCall.getDebugLoc());
}

llvm::Function *createModel(llvm::Module &M, llvm::Function &Broker,
                            llvm::CallBase &BrokerCall,
                            llvm::ArrayRef<CallbackSpec> Specs, size_t Id) {
  auto *ModelTy = createModelType(BrokerCall);
  auto *Model = llvm::Function::Create(
      ModelTy, llvm::GlobalValue::PrivateLinkage,
      createModelName(Broker, Id), &M);
  Model->setCallingConv(BrokerCall.getCallingConv());

  auto *EntryBB = llvm::BasicBlock::Create(Model->getContext(), "entry", Model);
  llvm::IRBuilder<> IRB(EntryBB);

  auto *BrokerRet = createOriginalBrokerCall(IRB, *Model, BrokerCall);

  for (const auto &Spec : Specs) {
    createCallbackCall(IRB, *Model, Broker, BrokerCall, Spec);
  }

  if (ModelTy->getReturnType()->isVoidTy()) {
    IRB.CreateRetVoid();
  } else {
    IRB.CreateRet(adaptValue(IRB, BrokerRet, ModelTy->getReturnType()));
  }

  return Model;
}

bool shouldSkipFunction(const llvm::Function &F) {
  return F.isDeclaration() || ExternCallbackModel::isModelGenerated(F);
}

} // namespace

size_t ExternCallbackModel::rewriteCalls(llvm::Module &M) {
  llvm::SmallVector<RewriteCandidate, 8> BrokerCalls;
  size_t Counter = 0;

  for (auto &F : M) {
    if (shouldSkipFunction(F)) {
      continue;
    }

    for (auto &Inst : llvm::instructions(F)) {
      auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst);
      if (!Call) {
        continue;
      }

      auto *Broker = llvm::dyn_cast<llvm::Function>(
          Call->getCalledOperand()->stripPointerCastsAndAliases());
      if (!Broker) {
        continue;
      }

      auto Specs = getCallbackSpecs(*Broker);
      if (Specs.empty() || !validateSpecs(*Broker, *Call, Specs)) {
        continue;
      }

      BrokerCalls.push_back({Call, Broker, std::move(Specs), Counter++});
    }
  }

  for (auto &Candidate : BrokerCalls) {
    auto *Model = createModel(M, *Candidate.Broker, *Candidate.Call,
                              Candidate.Specs, Candidate.Id);
    Candidate.Call->setCalledFunction(Model->getFunctionType(), Model);
    Candidate.Call->setAttributes(llvm::AttributeList{});
  }

  return BrokerCalls.size();
}

bool ExternCallbackModel::isModelGenerated(const llvm::Function &F) noexcept {
  return F.hasName() && F.getName().startswith(ModelPrefix);
}

} // namespace lotus
