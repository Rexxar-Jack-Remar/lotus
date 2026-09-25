#include "Analysis/CFG/GlobalCtorsDtorsModel.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <map>

namespace lotus {

namespace {

constexpr llvm::StringLiteral GlobalCtorsName = "llvm.global_ctors";
constexpr llvm::StringLiteral GlobalDtorsName = "llvm.global_dtors";

template <typename HandlerFn>
void forEachGlobalCtorDtor(const llvm::Module &M, llvm::StringRef GlobalName,
                           HandlerFn Handler) {
  const auto *Gtors = M.getGlobalVariable(GlobalName);
  if (!Gtors) {
    return;
  }

  if (!llvm::isa<llvm::ArrayType>(Gtors->getValueType()) ||
      !Gtors->hasInitializer()) {
    return;
  }

  const auto *ConstFunArray =
      llvm::dyn_cast<llvm::ConstantArray>(Gtors->getInitializer());
  if (!ConstFunArray) {
    return;
  }

  for (const auto &Op : ConstFunArray->operands()) {
    const auto *FunDesc = llvm::dyn_cast<llvm::ConstantStruct>(Op);
    if (!FunDesc) {
      continue;
    }

    auto *Fun = llvm::dyn_cast<llvm::Function>(
        FunDesc->getOperand(1)->stripPointerCastsAndAliases());
    const auto *Prio =
        llvm::dyn_cast<llvm::ConstantInt>(FunDesc->getOperand(0));
    if (Fun && Prio) {
      Handler(size_t(Prio->getLimitedValue(SIZE_MAX)), Fun);
    }
  }
}

std::multimap<size_t, llvm::Function *>
collectGlobalCtors(const llvm::Module &Mod) {
  std::multimap<size_t, llvm::Function *> Ret;
  forEachGlobalCtorDtor(
      Mod, GlobalCtorsName,
      [&Ret](size_t Prio, llvm::Function *Fun) { Ret.emplace(Prio, Fun); });
  return Ret;
}

std::multimap<size_t, llvm::Function *, std::greater<>>
collectGlobalDtors(const llvm::Module &Mod) {
  std::multimap<size_t, llvm::Function *, std::greater<>> Ret;
  forEachGlobalCtorDtor(
      Mod, GlobalDtorsName,
      [&Ret](size_t Prio, llvm::Function *Fun) { Ret.emplace(Prio, Fun); });
  return Ret;
}

llvm::SmallVector<llvm::FunctionCallee, 4>
collectRegisteredDtorsAtExit(const llvm::Module &Mod) {
  llvm::SmallVector<llvm::FunctionCallee, 4> Ret;
  auto *AtExitFn = Mod.getFunction("atexit");
  if (!AtExitFn) {
    return Ret;
  }

  for (auto *User : AtExitFn->users()) {
    auto *Call = llvm::dyn_cast<llvm::CallBase>(User);
    if (!Call || Call->arg_empty()) {
      continue;
    }

    auto *DtorOp = llvm::dyn_cast_or_null<llvm::Function>(
        Call->getArgOperand(0)->stripPointerCastsAndAliases());
    if (DtorOp) {
      Ret.push_back(DtorOp);
    }
  }

  return Ret;
}

llvm::SmallVector<std::pair<llvm::FunctionCallee, llvm::Value *>, 4>
collectRegisteredDtorsForModule(const llvm::Module &Mod) {
  llvm::SmallVector<std::pair<llvm::FunctionCallee, llvm::Value *>, 4>
      RegisteredDtors, RegisteredLocalStaticDtors;

  auto *CxaAtExitFn = Mod.getFunction("__cxa_atexit");
  if (!CxaAtExitFn) {
    return RegisteredDtors;
  }

  for (auto *User : CxaAtExitFn->users()) {
    auto *Call = llvm::dyn_cast<llvm::CallBase>(User);
    if (!Call || Call->arg_size() < 2) {
      continue;
    }

    auto *DtorOp = llvm::dyn_cast_or_null<llvm::Function>(
        Call->getArgOperand(0)->stripPointerCastsAndAliases());
    auto *DtorArgOp = Call->getArgOperand(1)->stripPointerCastsAndAliases();

    if (!DtorOp || !DtorArgOp) {
      continue;
    }

    if (Call->getFunction()->getName().contains("__cxx_global_var_init")) {
      RegisteredDtors.emplace_back(DtorOp, DtorArgOp);
    } else {
      RegisteredLocalStaticDtors.emplace_back(DtorOp, DtorArgOp);
    }
  }

  RegisteredDtors.append(RegisteredLocalStaticDtors.begin(),
                         RegisteredLocalStaticDtors.end());

  return RegisteredDtors;
}

std::string getReducedModuleName(const llvm::Module &M) {
  auto Name = M.getName().str();
  if (auto Idx = Name.find_last_of('/'); Idx != std::string::npos) {
    Name.erase(0, Idx + 1);
  }
  return Name;
}

llvm::Function *createDtorCallerForModule(
    llvm::Module &Mod,
    llvm::ArrayRef<std::pair<llvm::FunctionCallee, llvm::Value *>>
        RegisteredDtors,
    llvm::ArrayRef<llvm::FunctionCallee> RegisteredDtorsAtExit) {
  auto *DtorCaller = llvm::cast<llvm::Function>(
      Mod.getOrInsertFunction((GlobalCtorsDtorsModel::DtorsCallerName +
                               llvm::Twine('.') + getReducedModuleName(Mod))
                                  .str(),
                              llvm::Type::getVoidTy(Mod.getContext()))
          .getCallee());

  if (!DtorCaller->empty()) {
    return DtorCaller;
  }

  auto *BB =
      llvm::BasicBlock::Create(Mod.getContext(), "entry", DtorCaller);
  llvm::IRBuilder<> IRB(BB);

  for (auto FunCallee : llvm::reverse(RegisteredDtorsAtExit)) {
    IRB.CreateCall(FunCallee, {});
  }

  for (auto [FunCallee, Arg] : llvm::reverse(RegisteredDtors)) {
    if (FunCallee.getFunctionType()->getNumParams() != 1) {
      continue;
    }
    auto *ExpectedArgType = FunCallee.getFunctionType()->getParamType(0);

    if (Arg->getType() != ExpectedArgType) {
      if (!Arg->getType()->canLosslesslyBitCastTo(ExpectedArgType)) {
        continue;
      }
      Arg = IRB.CreateBitOrPointerCast(Arg, ExpectedArgType);
    }
    IRB.CreateCall(FunCallee, {Arg});
  }

  IRB.CreateRetVoid();
  return DtorCaller;
}

llvm::Function *collectRegisteredDtors(
    std::multimap<size_t, llvm::Function *, std::greater<>> &GlobalDtors,
    llvm::Module &Mod) {
  auto RegisteredDtors = collectRegisteredDtorsForModule(Mod);
  auto RegisteredDtorsAtExit = collectRegisteredDtorsAtExit(Mod);

  if (RegisteredDtors.empty() && RegisteredDtorsAtExit.empty()) {
    return nullptr;
  }

  auto *RegisteredDtorCaller =
      createDtorCallerForModule(Mod, RegisteredDtors, RegisteredDtorsAtExit);
  GlobalDtors.emplace(0, RegisteredDtorCaller);
  return RegisteredDtorCaller;
}

std::pair<llvm::Function *, bool> buildCRuntimeGlobalDtorsModel(
    llvm::Module &M,
    const std::multimap<size_t, llvm::Function *, std::greater<>>
        &GlobalDtors) {
  if (GlobalDtors.size() == 1) {
    return {GlobalDtors.begin()->second, false};
  }

  auto &Ctx = M.getContext();
  auto *Cleanup = llvm::cast<llvm::Function>(
      M.getOrInsertFunction(GlobalCtorsDtorsModel::DtorModelName,
                            llvm::Type::getVoidTy(Ctx))
          .getCallee());

  if (!Cleanup->empty()) {
    return {Cleanup, false};
  }

  auto *EntryBB = llvm::BasicBlock::Create(Ctx, "entry", Cleanup);
  llvm::IRBuilder<> IRB(EntryBB);

  for (auto [unused, Dtor] : GlobalDtors) {
    if (Dtor && Dtor->arg_empty()) {
      IRB.CreateCall(Dtor);
    }
  }

  IRB.CreateRetVoid();
  return {Cleanup, true};
}

std::string typeToString(const llvm::Type *Ty) {
  std::string Str;
  llvm::raw_string_ostream OS(Str);
  Ty->print(OS);
  return OS.str();
}

class NonDetValueBuilder {
public:
  explicit NonDetValueBuilder(llvm::Module &M) noexcept : M(&M) {}

  llvm::Value *createValue(llvm::IRBuilder<> &IRB, llvm::Type *Ty) {
    auto &Producer = Producers[Ty];
    if (!Producer) {
      Producer =
          M->getOrInsertFunction((GlobalCtorsDtorsModel::NonDetValuePrefix +
                                  llvm::Twine('.') + typeToString(Ty))
                                     .str(),
                                 Ty);
    }
    return IRB.CreateCall(Producer);
  }

  llvm::Value *createArgument(llvm::IRBuilder<> &IRB,
                              const llvm::Function &UEntry, unsigned ArgNo) {
    if (auto *ByValTy = UEntry.getParamByValType(ArgNo)) {
      auto *Buffer = IRB.CreateAlloca(ByValTy);
      IRB.CreateStore(createValue(IRB, ByValTy), Buffer);
      return Buffer;
    }

    if (auto *SRetTy = UEntry.getParamStructRetType(ArgNo)) {
      return IRB.CreateAlloca(SRetTy);
    }

    return createValue(IRB, UEntry.getArg(ArgNo)->getType());
  }

private:
  llvm::Module *M;
  llvm::DenseMap<llvm::Type *, llvm::FunctionCallee> Producers;
};

llvm::Function *buildExitModel(llvm::Module &M,
                               llvm::Function *GlobalCleanupFn) {
  auto *ExitFn = M.getFunction("exit");
  if (!ExitFn || !ExitFn->isDeclaration()) {
    return nullptr;
  }

  auto *BB = llvm::BasicBlock::Create(M.getContext(), "entry", ExitFn);
  llvm::IRBuilder<> IRB(BB);
  IRB.CreateCall(GlobalCleanupFn);
  IRB.CreateUnreachable();

  return ExitFn;
}

} // namespace

llvm::DenseSet<const llvm::Function *>
GlobalCtorsDtorsModel::collectGlobalCtorsDtors(const llvm::Module &M) {
  llvm::DenseSet<const llvm::Function *> Ret;
  const auto Insert = [&Ret](size_t, llvm::Function *Fun) { Ret.insert(Fun); };
  forEachGlobalCtorDtor(M, GlobalCtorsName, Insert);
  forEachGlobalCtorDtor(M, GlobalDtorsName, Insert);
  return Ret;
}

llvm::Function *GlobalCtorsDtorsModel::buildModel(
    llvm::Module &M, llvm::ArrayRef<llvm::Function *> UserEntryPoints) {
  if (auto *Existing = M.getFunction(ModelName)) {
    if (!Existing->empty()) {
      return Existing;
    }
  }

  auto GlobalCtors = collectGlobalCtors(M);
  auto GlobalDtors = collectGlobalDtors(M);
  collectRegisteredDtors(GlobalDtors, M);

  auto [GlobalCleanupFn, Inserted] =
      buildCRuntimeGlobalDtorsModel(M, GlobalDtors);

  if (!GlobalDtors.empty()) {
    buildExitModel(M, GlobalCleanupFn);
  }

  auto &Ctx = M.getContext();
  auto *GlobModel = llvm::cast<llvm::Function>(
      M.getOrInsertFunction(ModelName,
                            llvm::Type::getVoidTy(Ctx),
                            llvm::Type::getInt32Ty(Ctx),
                            llvm::Type::getInt8PtrTy(Ctx)->getPointerTo())
          .getCallee());

  auto *EntryBB = llvm::BasicBlock::Create(Ctx, "entry", GlobModel);
  llvm::IRBuilder<> IRB(EntryBB);

  for (auto [unused, Ctor] : GlobalCtors) {
    if (Ctor && Ctor->arg_empty()) {
      IRB.CreateCall(Ctor);
    }
  }

  NonDetValueBuilder NonDet(M);

  const auto CallUEntry = [&](llvm::Function *UEntry) {
    auto NumArgs = UEntry->arg_size();
    bool IsMain = UEntry->getName() == "main";

    llvm::SmallVector<llvm::Value *, 4> Args;
    Args.reserve(NumArgs);

    for (unsigned ArgNo = 0; ArgNo != NumArgs; ++ArgNo) {
      auto *ArgTy = UEntry->getArg(ArgNo)->getType();
      if (IsMain && ArgNo < 2 && ArgTy == GlobModel->getArg(ArgNo)->getType()) {
        Args.push_back(GlobModel->getArg(ArgNo));
        continue;
      }

      Args.push_back(NonDet.createArgument(IRB, *UEntry, ArgNo));
    }

    auto *Call = IRB.CreateCall(UEntry, Args);

    for (unsigned ArgNo = 0; ArgNo != NumArgs; ++ArgNo) {
      for (const auto &Attr : UEntry->getAttributes().getParamAttrs(ArgNo)) {
        Call->addParamAttr(ArgNo, Attr);
      }
    }
  };

  if (UserEntryPoints.size() <= 1) {
    if (!UserEntryPoints.empty() && UserEntryPoints.front()) {
      CallUEntry(UserEntryPoints.front());
    }
    IRB.CreateCall(GlobalCleanupFn);
    IRB.CreateRetVoid();
    return GlobModel;
  }

  auto UEntrySelectorFn =
      M.getOrInsertFunction(UserEntrySelectorName, llvm::Type::getInt32Ty(Ctx));

  auto *UEntrySelector = IRB.CreateCall(UEntrySelectorFn);
  auto *DefaultBB = llvm::BasicBlock::Create(Ctx, "invalid", GlobModel);
  auto *SwitchEnd = llvm::BasicBlock::Create(Ctx, "switchEnd", GlobModel);

  auto *UEntrySwitch =
      IRB.CreateSwitch(UEntrySelector, DefaultBB, UserEntryPoints.size());

  IRB.SetInsertPoint(DefaultBB);
  IRB.CreateUnreachable();

  for (size_t Idx = 0; Idx < UserEntryPoints.size(); ++Idx) {
    auto *UEntry = UserEntryPoints[Idx];
    if (!UEntry) {
      continue;
    }
    auto *BB =
        llvm::BasicBlock::Create(Ctx, "call." + UEntry->getName(), GlobModel);
    IRB.SetInsertPoint(BB);
    CallUEntry(UEntry);
    IRB.CreateBr(SwitchEnd);

    UEntrySwitch->addCase(IRB.getInt32(uint32_t(Idx)), BB);
  }

  IRB.SetInsertPoint(SwitchEnd);
  IRB.CreateCall(GlobalCleanupFn);
  IRB.CreateRetVoid();

  return GlobModel;
}

llvm::Function *
GlobalCtorsDtorsModel::buildModel(llvm::Module &M,
                                  llvm::ArrayRef<std::string> UserEntryPoints) {
  llvm::SmallVector<llvm::Function *, 4> Fns;
  for (const auto &Name : UserEntryPoints) {
    if (auto *F = M.getFunction(Name)) {
      if (!F->isDeclaration()) {
        Fns.push_back(F);
      }
    }
  }
  if (Fns.empty()) {
    for (auto &F : M) {
      if (!F.isDeclaration() && F.hasExternalLinkage() &&
          !isModelGenerated(F)) {
        Fns.push_back(&F);
      }
    }
  }
  return buildModel(M, Fns);
}

bool GlobalCtorsDtorsModel::isModelGenerated(
    const llvm::Function &F) noexcept {
  if (!F.hasName()) {
    return false;
  }

  llvm::StringRef FunctionName = F.getName();
  const auto Cases = {ModelName, DtorModelName, DtorsCallerName,
                      UserEntrySelectorName, NonDetValuePrefix};
  return llvm::any_of(Cases, [FunctionName](llvm::StringLiteral Case) {
    return FunctionName.startswith(Case);
  });
}

} // namespace lotus
