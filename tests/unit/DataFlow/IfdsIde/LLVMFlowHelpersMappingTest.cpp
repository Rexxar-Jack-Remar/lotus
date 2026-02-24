#include <memory>
#include <set>

#include <Dataflow/IFDS/Utils/LLVMFlowHelpers.h>
#include <gtest/gtest.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

namespace {

struct MappingFixtureIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  const llvm::CallBase *Call = nullptr;
  const llvm::Function *Callee = nullptr;
  const llvm::Value *ActualArg = nullptr;
  const llvm::Argument *FormalArg = nullptr;
};

MappingFixtureIR buildMappingFixture() {
  MappingFixtureIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod =
      std::make_unique<llvm::Module>("llvm_flow_helper_mapping_test", *IR.Ctx);

  auto *I32Ty = llvm::Type::getInt32Ty(*IR.Ctx);

  auto *CalleeTy = llvm::FunctionType::get(I32Ty, {I32Ty}, false);
  auto *Callee = llvm::Function::Create(
      CalleeTy, llvm::Function::ExternalLinkage, "callee", IR.Mod.get());

  auto *MainTy = llvm::FunctionType::get(I32Ty, {}, false);
  auto *Main = llvm::Function::Create(MainTy, llvm::Function::ExternalLinkage,
                                      "main", IR.Mod.get());

  auto *MainEntry = llvm::BasicBlock::Create(*IR.Ctx, "entry", Main);
  llvm::IRBuilder<> BMain(MainEntry);
  auto *Actual = BMain.CreateAdd(llvm::ConstantInt::get(I32Ty, 1),
                                 llvm::ConstantInt::get(I32Ty, 2), "actual");
  auto *Call = BMain.CreateCall(Callee, {Actual});
  BMain.CreateRet(Call);

  auto *CalleeEntry = llvm::BasicBlock::Create(*IR.Ctx, "entry", Callee);
  llvm::IRBuilder<> BCallee(CalleeEntry);
  BCallee.CreateRet(Callee->getArg(0));

  IR.Call = Call;
  IR.Callee = Callee;
  IR.ActualArg = Actual;
  IR.FormalArg = &*Callee->arg_begin();
  return IR;
}

} // namespace

TEST(LLVMFlowHelpersMappingTest, MapFactsToCalleeMatchesAndMaps) {
  auto IR = buildMappingFixture();
  std::set<const llvm::Value *> Out;
  const llvm::Value *Source = IR.ActualArg;

  ifds::flow::map_facts_to_callee(
      IR.Call, IR.Callee, Source, Out,
      [](const llvm::Value *Actual, const llvm::Argument * /*Formal*/,
         const llvm::Value *Fact) { return Actual == Fact; },
      [](const llvm::Value * /*Actual*/, const llvm::Argument *Formal,
         const llvm::Value * /*Fact*/) -> const llvm::Value * {
        return Formal;
      });

  EXPECT_EQ(Out.size(), 1U);
  EXPECT_TRUE(Out.count(IR.FormalArg));
}

TEST(LLVMFlowHelpersMappingTest, MapFactsToCalleeNoMatchProducesNothing) {
  auto IR = buildMappingFixture();
  std::set<const llvm::Value *> Out;
  const llvm::Value *Source =
      llvm::ConstantInt::get(llvm::Type::getInt32Ty(*IR.Ctx), 99);

  ifds::flow::map_facts_to_callee(
      IR.Call, IR.Callee, Source, Out,
      [](const llvm::Value *Actual, const llvm::Argument * /*Formal*/,
         const llvm::Value *Fact) { return Actual == Fact; },
      [](const llvm::Value * /*Actual*/, const llvm::Argument *Formal,
         const llvm::Value * /*Fact*/) -> const llvm::Value * {
        return Formal;
      });

  EXPECT_TRUE(Out.empty());
}

TEST(LLVMFlowHelpersMappingTest, MapFactsToCallerMapsFormalBackToActual) {
  auto IR = buildMappingFixture();
  std::set<const llvm::Value *> Out;
  const llvm::Value *Source = IR.FormalArg;

  ifds::flow::map_facts_to_caller(
      IR.Call, IR.Callee, Source, Out,
      [](const llvm::Argument *Formal, const llvm::Value * /*Actual*/,
         const llvm::Value *Fact) { return Formal == Fact; },
      [](const llvm::Argument * /*Formal*/, const llvm::Value *Actual,
         const llvm::Value * /*Fact*/) -> const llvm::Value * {
        return Actual;
      },
      [](const llvm::Value * /*RetVal*/, const llvm::Value * /*Fact*/) {
        return false;
      },
      [](const llvm::Value *RetVal, const llvm::Value * /*Fact*/)
          -> const llvm::Value * { return RetVal; });

  EXPECT_EQ(Out.size(), 1U);
  EXPECT_TRUE(Out.count(IR.ActualArg));
}

TEST(LLVMFlowHelpersMappingTest, MapFactsToCallerMapsReturnValueToCall) {
  auto IR = buildMappingFixture();
  std::set<const llvm::Value *> Out;
  const llvm::Value *Source = IR.FormalArg;

  ifds::flow::map_facts_to_caller(
      IR.Call, IR.Callee, Source, Out,
      [](const llvm::Argument * /*Formal*/, const llvm::Value * /*Actual*/,
         const llvm::Value * /*Fact*/) { return false; },
      [](const llvm::Argument * /*Formal*/, const llvm::Value *Actual,
         const llvm::Value * /*Fact*/) -> const llvm::Value * {
        return Actual;
      },
      [](const llvm::Value *RetVal, const llvm::Value *Fact) {
        return RetVal == Fact;
      },
      [Call = IR.Call](const llvm::Value * /*RetVal*/,
                       const llvm::Value * /*Fact*/) -> const llvm::Value * {
        return Call;
      });

  EXPECT_EQ(Out.size(), 1U);
  EXPECT_TRUE(Out.count(IR.Call));
}
