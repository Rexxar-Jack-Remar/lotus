#include "Analysis/Purity/FunctionPurityAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using lotus::analysis::purity::FunctionPurityAnalysis;
using lotus::analysis::purity::PurityKind;
using lotus::unittest::parseModuleChecked;

namespace {

TEST(FunctionPurityAnalysisTest, ClassifiesPureReadonlyAndImpureFunctions) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @pure_add(i32 %x, i32 %y) {
    entry:
      %sum = add i32 %x, %y
      ret i32 %sum
    }

    define i32 @readonly_load(i32* %p) {
    entry:
      %v = load i32, i32* %p, align 4
      ret i32 %v
    }

    define void @impure_store(i32* %p) {
    entry:
      store i32 7, i32* %p, align 4
      ret void
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  EXPECT_EQ(analysis.getPurity(module->getFunction("pure_add")),
            PurityKind::Pure);
  EXPECT_EQ(analysis.getPurity(module->getFunction("readonly_load")),
            PurityKind::ReadOnly);
  EXPECT_EQ(analysis.getPurity(module->getFunction("impure_store")),
            PurityKind::Impure);
}

TEST(FunctionPurityAnalysisTest, PropagatesPurityInterprocedurally) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @leaf(i32 %x) {
    entry:
      %r = mul i32 %x, 3
      ret i32 %r
    }

    define i32 @wrapper(i32 %x) {
    entry:
      %call = call i32 @leaf(i32 %x)
      %inc = add i32 %call, 1
      ret i32 %inc
    }

    define i32 @reader(i32* %p) {
    entry:
      %v = load i32, i32* %p, align 4
      ret i32 %v
    }

    define i32 @reader_wrapper(i32* %p) {
    entry:
      %call = call i32 @reader(i32* %p)
      ret i32 %call
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  EXPECT_TRUE(analysis.isPure(module->getFunction("leaf")));
  EXPECT_TRUE(analysis.isPure(module->getFunction("wrapper")));
  EXPECT_TRUE(analysis.isReadOnly(module->getFunction("reader")));
  EXPECT_TRUE(analysis.isReadOnly(module->getFunction("reader_wrapper")));
}

TEST(FunctionPurityAnalysisTest, HandlesRecursionAndExternalCallsConservatively) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    declare i32 @reader_ext(i8*) readonly
    declare i32 @unknown(i32*)

    define i32 @recursive(i32 %n) {
    entry:
      %cond = icmp eq i32 %n, 0
      br i1 %cond, label %base, label %step

    base:
      ret i32 1

    step:
      %dec = sub i32 %n, 1
      %call = call i32 @recursive(i32 %dec)
      %sum = add i32 %call, %n
      ret i32 %sum
    }

    define i32 @reader_ext_wrapper(i8* %p) {
    entry:
      %call = call i32 @reader_ext(i8* %p)
      ret i32 %call
    }

    define i32 @unknown_wrapper(i32* %p) {
    entry:
      %call = call i32 @unknown(i32* %p)
      ret i32 %call
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  EXPECT_EQ(analysis.getPurity(module->getFunction("recursive")),
            PurityKind::Pure);
  EXPECT_EQ(analysis.getPurity(module->getFunction("reader_ext_wrapper")),
            PurityKind::ReadOnly);
  EXPECT_EQ(analysis.getPurity(module->getFunction("unknown_wrapper")),
            PurityKind::Impure);
}

TEST(FunctionPurityAnalysisTest, TreatsIndirectCallsAsImpure) {
  LLVMContext context;
  auto module = parseModuleChecked(context, R"(
    define i32 @indirect(i32 (i32)* %fp, i32 %x) {
    entry:
      %call = call i32 %fp(i32 %x)
      ret i32 %call
    }
  )", "FunctionPurityAnalysisTest");

  FunctionPurityAnalysis analysis(*module);
  analysis.run();

  EXPECT_EQ(analysis.getPurity(module->getFunction("indirect")),
            PurityKind::Impure);
}

} // namespace
