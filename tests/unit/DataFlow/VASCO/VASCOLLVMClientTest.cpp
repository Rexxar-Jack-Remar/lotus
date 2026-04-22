#include "Dataflow/VASCO/VASCO.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace {

class VASCOLLVMClientTest : public ::testing::Test {
protected:
  llvm::LLVMContext Context;

  std::unique_ptr<llvm::Module> parse(const char *IR) {
    return lotus::unittest::parseModuleChecked(Context, IR, "VASCOTest");
  }
};

TEST_F(VASCOLLVMClientTest, SignAnalysisTracksRecursiveExample) {
  auto Module = parse(R"(
    define i32 @five() {
    entry:
      ret i32 5
    }

    define i32 @f(i32 %a, i32 %b) {
    entry:
      %cmp = icmp slt i32 %a, %b
      br i1 %cmp, label %mulb, label %callb
    mulb:
      %c.mul = mul i32 %a, %b
      br label %ret
    callb:
      %c.call = call i32 @g(i32 10)
      br label %ret
    ret:
      %c = phi i32 [ %c.mul, %mulb ], [ %c.call, %callb ]
      ret i32 %c
    }

    define i32 @g(i32 %u) {
    entry:
      %neg = sub i32 0, %u
      %v = call i32 @f(i32 %neg, i32 %u)
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %p = call i32 @five()
      %q = call i32 @f(i32 %p, i32 -3)
      %negq = sub i32 0, %q
      %r = call i32 @g(i32 %negq)
      ret i32 %r
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::SignAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *F = Module->getFunction("f");
  auto *G = Module->getFunction("g");
  ASSERT_NE(F, nullptr);
  ASSERT_NE(G, nullptr);

  const auto &FContexts = Analysis.getContexts(F);
  const auto &GContexts = Analysis.getContexts(G);
  ASSERT_EQ(FContexts.size(), 2U);
  ASSERT_EQ(GContexts.size(), 1U);

  auto ArgIt = F->arg_begin();
  auto *AArg = &*ArgIt++;
  auto *BArg = &*ArgIt++;
  EXPECT_EQ(
      FContexts[0]->getExitValue().at(vasco::llvmir::ValueKey::returnValue()),
      vasco::llvmir::Sign::Negative);
  EXPECT_EQ(
      FContexts[1]->getExitValue().at(vasco::llvmir::ValueKey::returnValue()),
      vasco::llvmir::Sign::Negative);

  bool SawPositiveNegative = false;
  bool SawNegativePositive = false;
  for (const auto &Context : FContexts) {
    const auto &Entry = Context->getEntryValue();
    auto ASign = Entry.at(vasco::llvmir::ValueKey::forValue(AArg));
    auto BSign = Entry.at(vasco::llvmir::ValueKey::forValue(BArg));
    SawPositiveNegative |=
        ASign == vasco::llvmir::Sign::Positive &&
        BSign == vasco::llvmir::Sign::Negative;
    SawNegativePositive |=
        ASign == vasco::llvmir::Sign::Negative &&
        BSign == vasco::llvmir::Sign::Positive;
  }
  EXPECT_TRUE(SawPositiveNegative);
  EXPECT_TRUE(SawNegativePositive);

  auto *Main = Module->getFunction("main");
  auto *RCall = lotus::unittest::findInstructionByName(Main, "r");
  ASSERT_NE(RCall, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  EXPECT_EQ(
      Solution.getValueAfter(RCall).at(vasco::llvmir::ValueKey::forValue(RCall)),
      vasco::llvmir::Sign::Negative);
}

TEST_F(VASCOLLVMClientTest, CopyConstantAnalysisTracksInterproceduralReturns) {
  auto Module = parse(R"(
    define i32 @id(i32 %a) {
    entry:
      ret i32 %a
    }

    define i32 @foo(i32 %a, i32 %b, i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else
    then:
      br label %merge
    else:
      br label %merge
    merge:
      %z = phi i32 [ %a, %then ], [ %b, %else ]
      ret i32 %z
    }

    define i32 @main() {
    entry:
      %x = call i32 @id(i32 8)
      %y = call i32 @foo(i32 8, i32 8, i1 true)
      ret i32 %y
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::CopyConstantAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Foo = Module->getFunction("foo");
  ASSERT_NE(Foo, nullptr);
  const auto &FooContexts = Analysis.getContexts(Foo);
  ASSERT_EQ(FooContexts.size(), 1U);

  const llvm::Constant *RetConstant =
      FooContexts.front()->getExitValue().at(vasco::llvmir::ValueKey::returnValue());
  auto *RetInt = llvm::dyn_cast<llvm::ConstantInt>(RetConstant);
  ASSERT_NE(RetInt, nullptr);
  EXPECT_EQ(RetInt->getSExtValue(), 8);

  auto *Main = Module->getFunction("main");
  auto *YCall = lotus::unittest::findInstructionByName(Main, "y");
  ASSERT_NE(YCall, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  const llvm::Constant *YConstant =
      Solution.getValueAfter(YCall).at(vasco::llvmir::ValueKey::forValue(YCall));
  auto *YInt = llvm::dyn_cast<llvm::ConstantInt>(YConstant);
  ASSERT_NE(YInt, nullptr);
  EXPECT_EQ(YInt->getSExtValue(), 8);
}

TEST_F(VASCOLLVMClientTest, DefaultLLVMProgramRepresentationTreatsIndirectAsUnknown) {
  auto Module = parse(R"(
    define i32 @callee(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main(i32 (i32)* %fp) {
    entry:
      %r = call i32 %fp(i32 1)
      ret i32 %r
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get(),
                                                          {Module->getFunction("main")});
  auto *Main = Module->getFunction("main");
  auto *Call = llvm::dyn_cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(Main, "r"));
  ASSERT_NE(Call, nullptr);

  auto Targets = Program.resolveTargets(Main, Call);
  EXPECT_FALSE(Targets.has_value());
}

TEST_F(VASCOLLVMClientTest, DefaultLLVMProgramRepresentationResolvesAliasedDirectCall) {
  auto Module = parse(R"(
    define i32 @callee(i32 %x) {
    entry:
      ret i32 %x
    }

    @callee.alias = alias i32 (i32), i32 (i32)* @callee

    define i32 @main() {
    entry:
      %r = call i32 @callee.alias(i32 7)
      ret i32 %r
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  auto *Main = Module->getFunction("main");
  auto *Call = llvm::dyn_cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(Main, "r"));
  ASSERT_NE(Call, nullptr);

  auto Targets = Program.resolveTargets(Main, Call);
  ASSERT_TRUE(Targets.has_value());
  ASSERT_EQ(Targets->size(), 1U);
  EXPECT_EQ(Targets->front(), Module->getFunction("callee"));
}

TEST_F(VASCOLLVMClientTest, UnknownCallsConservativelyKillCopyConstantResult) {
  auto Module = parse(R"(
    define i32 @main(i32 (i32)* %fp) {
    entry:
      %x = call i32 %fp(i32 1)
      ret i32 %x
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get(),
                                                          {Module->getFunction("main")});
  vasco::llvmir::CopyConstantAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *XCall = lotus::unittest::findInstructionByName(Main, "x");
  ASSERT_NE(XCall, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  EXPECT_EQ(Solution.getValueAfter(XCall).at(vasco::llvmir::ValueKey::forValue(XCall)),
            nullptr);
}

TEST_F(VASCOLLVMClientTest, UnknownCallsConservativelyBottomSignResult) {
  auto Module = parse(R"(
    define i32 @main(i32 (i32)* %fp) {
    entry:
      %x = call i32 %fp(i32 1)
      ret i32 %x
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get(),
                                                          {Module->getFunction("main")});
  vasco::llvmir::SignAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *Main = Module->getFunction("main");
  auto *XCall = lotus::unittest::findInstructionByName(Main, "x");
  ASSERT_NE(XCall, nullptr);

  const auto Solution = Analysis.getMeetOverValidPathsSolution();
  EXPECT_EQ(Solution.getValueAfter(XCall).at(vasco::llvmir::ValueKey::forValue(XCall)),
            vasco::llvmir::Sign::Bottom);
}

TEST_F(VASCOLLVMClientTest, SignAnalysisHandlesUnreachableExitAsTail) {
  auto Module = parse(R"(
    define i32 @abort_like() {
    entry:
      unreachable
    }

    define i32 @main() {
    entry:
      %r = call i32 @abort_like()
      ret i32 0
    }
  )");

  vasco::llvmir::DefaultLLVMProgramRepresentation Program(Module.get());
  vasco::llvmir::SignAnalysis Analysis(Program);
  Analysis.doAnalysis();

  auto *AbortLike = Module->getFunction("abort_like");
  ASSERT_NE(AbortLike, nullptr);

  const auto &Contexts = Analysis.getContexts(AbortLike);
  ASSERT_EQ(Contexts.size(), 1U);
  EXPECT_TRUE(Contexts.front()->isAnalysed());
}

} // namespace
