#include <memory>
#include <set>

#include <Dataflow/IFDS/Utils/LLVMFlowHelpers.h>
#include <gtest/gtest.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>

namespace {

struct CallFixtureIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  const llvm::CallBase *Call = nullptr;
  const llvm::Value *Arg = nullptr;
  const llvm::GlobalVariable *Global = nullptr;
};

CallFixtureIR buildCallFixture() {
  CallFixtureIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = std::make_unique<llvm::Module>("llvm_flow_helper_test", *IR.Ctx);

  auto *I32Ty = llvm::Type::getInt32Ty(*IR.Ctx);
  auto *PtrTy = llvm::Type::getInt32PtrTy(*IR.Ctx);

  IR.Global = new llvm::GlobalVariable(*IR.Mod, I32Ty, false,
                                       llvm::GlobalValue::ExternalLinkage,
                                       llvm::ConstantInt::get(I32Ty, 0), "g");

  auto *CalleeTy =
      llvm::FunctionType::get(llvm::Type::getVoidTy(*IR.Ctx), {PtrTy}, false);
  auto *Callee = llvm::Function::Create(
      CalleeTy, llvm::Function::ExternalLinkage, "callee", IR.Mod.get());

  auto *MainTy = llvm::FunctionType::get(I32Ty, {}, false);
  auto *Main = llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage,
                                      "main", IR.Mod.get());

  auto *Entry = llvm::BasicBlock::Create(*IR.Ctx, "entry", Main);
  llvm::IRBuilder<> B(Entry);
  auto *Local = B.CreateAlloca(I32Ty, nullptr, "local");
  IR.Arg = Local;
  IR.Call = B.CreateCall(Callee, {Local});
  B.CreateRet(llvm::ConstantInt::get(I32Ty, 0));

  return IR;
}

} // namespace

TEST(LLVMFlowHelpersTest, PolicyPropagatesZeroWhenEnabled) {
  auto IR = buildCallFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_alongside_callsite_with_policies(
      IR.Call, nullptr, Out,
      [](const llvm::Value *, const llvm::Value *) { return true; },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/true);

  EXPECT_EQ(Out.size(), 1U);
  EXPECT_TRUE(Out.count(nullptr));
}

TEST(LLVMFlowHelpersTest, PolicyKillsZeroWhenDisabled) {
  auto IR = buildCallFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_alongside_callsite_with_policies(
      IR.Call, nullptr, Out,
      [](const llvm::Value *, const llvm::Value *) { return false; },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/false);

  EXPECT_TRUE(Out.empty());
}

TEST(LLVMFlowHelpersTest, PolicyPropagatesGlobalWhenEnabled) {
  auto IR = buildCallFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_alongside_callsite_with_policies(
      IR.Call, IR.Global, Out,
      [](const llvm::Value *, const llvm::Value *) { return true; },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/true);

  EXPECT_EQ(Out.size(), 1U);
  EXPECT_TRUE(Out.count(IR.Global));
}

TEST(LLVMFlowHelpersTest, PolicyKillsGlobalWhenDisabled) {
  auto IR = buildCallFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_alongside_callsite_with_policies(
      IR.Call, IR.Global, Out,
      [](const llvm::Value *, const llvm::Value *) { return false; },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/false,
      /*PropagateZero=*/true);

  EXPECT_TRUE(Out.empty());
}

TEST(LLVMFlowHelpersTest, PolicyKillsLocalWhenPredicateMatches) {
  auto IR = buildCallFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_alongside_callsite_with_policies(
      IR.Call, IR.Arg, Out,
      [](const llvm::Value *Arg, const llvm::Value *Source) {
        return Arg == Source;
      },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/true);

  EXPECT_TRUE(Out.empty());
}

TEST(LLVMFlowHelpersTest, PolicyPropagatesLocalWhenPredicateDoesNotMatch) {
  auto IR = buildCallFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_alongside_callsite_with_policies(
      IR.Call, IR.Arg, Out,
      [](const llvm::Value *, const llvm::Value *) { return false; },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/true);

  EXPECT_EQ(Out.size(), 1U);
  EXPECT_TRUE(Out.count(IR.Arg));
}
