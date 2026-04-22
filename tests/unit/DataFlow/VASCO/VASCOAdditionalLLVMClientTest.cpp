#include "Dataflow/VASCO/VASCO.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace {

class VASCOAdditionalLLVMClientTest : public ::testing::Test {
protected:
  llvm::LLVMContext Context;

  std::unique_ptr<llvm::Module> parse(const char *IR) {
    return lotus::unittest::parseModuleChecked(Context, IR, "VASCOExtraTest");
  }
};

TEST_F(VASCOAdditionalLLVMClientTest, LiveVariablesAnalysisPropagatesUsedReturn) {
  auto Module = parse(R"(
    define i32 @id(i32 %a) {
    entry:
      ret i32 %a
    }

    define i32 @use(i32 %x) {
    entry:
      %live = call i32 @id(i32 %x)
      ret i32 %live
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get(),
                                                          {Module->getFunction("use")});
  vasco::llvmir::LiveVariablesAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Id = Module->getFunction("id");
  ASSERT_NE(Id, nullptr);

  const auto &Contexts = Analysis.getContexts(Id);
  ASSERT_EQ(Contexts.size(), 1U);

  auto *Arg = &*Id->arg_begin();
  EXPECT_NE(Contexts.front()->getExitValue().count(vasco::llvmir::ValueKey::returnValue()),
            0U);
  EXPECT_NE(Contexts.front()->getEntryValue().count(
                vasco::llvmir::ValueKey::forValue(Arg)),
            0U);
}

TEST_F(VASCOAdditionalLLVMClientTest, LiveVariablesAnalysisDropsUnusedReturn) {
  auto Module = parse(R"(
    define i32 @id(i32 %a) {
    entry:
      ret i32 %a
    }

    define i32 @discard(i32 %y) {
    entry:
      %dead = call i32 @id(i32 %y)
      ret i32 0
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(
      Module.get(), {Module->getFunction("discard")});
  vasco::llvmir::LiveVariablesAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Id = Module->getFunction("id");
  ASSERT_NE(Id, nullptr);

  const auto &Contexts = Analysis.getContexts(Id);
  ASSERT_EQ(Contexts.size(), 1U);

  auto *Arg = &*Id->arg_begin();
  EXPECT_EQ(Contexts.front()->getExitValue().count(vasco::llvmir::ValueKey::returnValue()),
            0U);
  EXPECT_EQ(Contexts.front()->getEntryValue().count(
                vasco::llvmir::ValueKey::forValue(Arg)),
            0U);
}

TEST_F(VASCOAdditionalLLVMClientTest,
       LiveVariablesAnalysisKeepsIndirectCallOperandsLive) {
  auto Module = parse(R"(
    define i32 @main(i32 (i32)* %fp, i32 %x) {
    entry:
      %r = call i32 %fp(i32 %x)
      ret i32 0
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get(),
                                                          {Module->getFunction("main")});
  vasco::llvmir::LiveVariablesAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *Call = lotus::unittest::findInstructionByName(Main, "r");
  ASSERT_NE(Call, nullptr);

  auto ArgIt = Main->arg_begin();
  auto *FpArg = &*ArgIt++;
  auto *XArg = &*ArgIt++;

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const auto &Before = Solution.getValueBefore(Call);
  const auto &After = Solution.getValueAfter(Call);

  EXPECT_NE(Before.count(vasco::llvmir::ValueKey::forValue(FpArg)), 0U);
  EXPECT_NE(Before.count(vasco::llvmir::ValueKey::forValue(XArg)), 0U);
  EXPECT_EQ(After.count(vasco::llvmir::ValueKey::forValue(Call)), 0U);
}

TEST_F(VASCOAdditionalLLVMClientTest,
       NullnessAnalysisTracksInterproceduralPointerReturn) {
  auto Module = parse(R"(
    define i8* @id(i8* %p) {
    entry:
      ret i8* %p
    }

    define i8* @main() {
    entry:
      %obj = alloca i8
      %ret = call i8* @id(i8* %obj)
      ret i8* %ret
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::NullnessAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *RetCall = lotus::unittest::findInstructionByName(Main, "ret");
  ASSERT_NE(RetCall, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  EXPECT_EQ(
      Solution.getValueAfter(RetCall).at(vasco::llvmir::ValueKey::forValue(RetCall)),
      vasco::llvmir::Nullness::NonNull);
}

TEST_F(VASCOAdditionalLLVMClientTest, NullnessAnalysisMergesNullAndNonNullFlows) {
  auto Module = parse(R"(
    define i8* @choose(i1 %cond, i8* %p) {
    entry:
      br i1 %cond, label %nullret, label %ptrret
    nullret:
      br label %merge
    ptrret:
      br label %merge
    merge:
      %r = phi i8* [ null, %nullret ], [ %p, %ptrret ]
      ret i8* %r
    }

    define i8* @main(i1 %cond) {
    entry:
      %obj = alloca i8
      %ret = call i8* @choose(i1 %cond, i8* %obj)
      ret i8* %ret
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::NullnessAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *RetCall = lotus::unittest::findInstructionByName(Main, "ret");
  ASSERT_NE(RetCall, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  EXPECT_EQ(
      Solution.getValueAfter(RetCall).at(vasco::llvmir::ValueKey::forValue(RetCall)),
      vasco::llvmir::Nullness::MaybeNull);
}

TEST_F(VASCOAdditionalLLVMClientTest,
       NullnessAnalysisTreatsUnknownPointerCallsConservatively) {
  auto Module = parse(R"(
    define i8* @main(i8* ()* %fp) {
    entry:
      %r = call i8* %fp()
      ret i8* %r
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get(),
                                                          {Module->getFunction("main")});
  vasco::llvmir::NullnessAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *Call = lotus::unittest::findInstructionByName(Main, "r");
  ASSERT_NE(Call, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  EXPECT_EQ(
      Solution.getValueAfter(Call).at(vasco::llvmir::ValueKey::forValue(Call)),
      vasco::llvmir::Nullness::MaybeNull);
}

} // namespace
