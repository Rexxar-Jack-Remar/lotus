#include <memory>
#include <set>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>
#include <Dataflow/IFDS/Utils/LLVMFlowHelpers.h>
#include <TestUtils/LLVMHelpers.h>

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
  IR.Mod = lotus::unittest::parseModule(*IR.Ctx, R"(
    define i32 @callee(i32 %arg) {
    entry:
      ret i32 %arg
    }

    define i32 @main() {
    entry:
      %actual = add i32 1, 2
      %call = call i32 @callee(i32 %actual)
      ret i32 %call
    }
  )", "LLVMFlowHelpersMappingTest");

  IR.Callee = IR.Mod->getFunction("callee");
  IR.Call = llvm::cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(*IR.Mod->getFunction("main"),
                                             "call"));
  IR.ActualArg =
      lotus::unittest::findInstructionByName(*IR.Mod->getFunction("main"),
                                             "actual");
  IR.FormalArg = IR.Callee != nullptr ? &*IR.Callee->arg_begin() : nullptr;
  return IR;
}

struct SRetFixtureIR {
  std::unique_ptr<llvm::LLVMContext> Ctx;
  std::unique_ptr<llvm::Module> Mod;
  const llvm::CallBase *Call = nullptr;
  const llvm::Function *Callee = nullptr;
  const llvm::Value *ActualArg = nullptr;
  const llvm::Argument *FormalArg = nullptr;
  const llvm::Instruction *ExitInst = nullptr;
};

SRetFixtureIR buildSRetFixture() {
  SRetFixtureIR IR;
  IR.Ctx = std::make_unique<llvm::LLVMContext>();
  IR.Mod = lotus::unittest::parseModule(*IR.Ctx, R"(
    %Tracked = type { i32 }

    define void @callee(%Tracked* sret(%Tracked) %out, i32* %arg) {
    entry:
      ret void
    }

    define void @main() {
    entry:
      %out = alloca %Tracked
      %actual = alloca i32
      call void @callee(%Tracked* sret(%Tracked) %out, i32* %actual)
      ret void
    }
  )", "LLVMFlowHelpersMappingSRetTest");

  IR.Callee = IR.Mod->getFunction("callee");
  IR.Call = lotus::unittest::findCallTo(*IR.Mod->getFunction("main"), "callee");
  IR.ActualArg =
      lotus::unittest::findInstructionByName(*IR.Mod->getFunction("main"),
                                             "actual");
  if (IR.Callee) {
    auto FormalIt = IR.Callee->arg_begin();
    if (FormalIt != IR.Callee->arg_end()) {
      ++FormalIt;
    }
    if (FormalIt != IR.Callee->arg_end()) {
      IR.FormalArg = &*FormalIt;
    }
    IR.ExitInst = IR.Callee->back().getTerminator();
  }
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

TEST(LLVMFlowHelpersMappingTest, MapFactsToCalleeWithPoliciesSkipsSRetFormal) {
  auto IR = buildSRetFixture();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_to_callee_with_policies(
      IR.Call, IR.Callee, IR.ActualArg, Out,
      [](const llvm::Value *Actual, const llvm::Argument * /*Formal*/,
         const llvm::Value *Fact) { return Actual == Fact; },
      [](const llvm::Value * /*Actual*/, const llvm::Argument *Formal,
         const llvm::Value * /*Fact*/) -> const llvm::Value * {
        return Formal;
      },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); });

  EXPECT_EQ(Out.size(), 1U);
  EXPECT_TRUE(Out.count(IR.FormalArg));
}

TEST(LLVMFlowHelpersMappingTest,
     MapFactsToCallerFromExitUsesSpecificExitAndPostProcess) {
  auto IR = buildMappingFixture();
  const llvm::Instruction *ExitInst =
      IR.Callee->back().getTerminator();
  std::set<const llvm::Value *> Out;

  ifds::flow::map_facts_to_caller_from_exit(
      IR.Call, ExitInst, IR.FormalArg, Out,
      [](const llvm::Argument *Formal, const llvm::Value * /*Actual*/,
         const llvm::Value *Fact) { return Formal == Fact; },
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
      },
      [](const llvm::Value *V) { return V == nullptr; },
      [](const llvm::Value *V) { return llvm::isa<llvm::GlobalValue>(V); },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/true,
      [](std::set<const llvm::Value *> &Facts) { Facts.insert(nullptr); });

  EXPECT_TRUE(Out.count(IR.ActualArg));
  EXPECT_TRUE(Out.count(IR.Call));
  EXPECT_TRUE(Out.count(nullptr));
}
