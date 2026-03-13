#include "Analysis/Concurrency/Memory/EscapeAnalysis.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus;

class EscapeAnalysisTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("EscapeAnalysisTest", errs());
    }
    return module;
  }
};

TEST_F(EscapeAnalysisTest, ExternalCallEscapesStackAddress) {
  const char *source = R"(
    declare void @external_sink(i8*)

    define void @main() {
    entry:
      %slot = alloca i8, align 1
      call void @external_sink(i8* %slot)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  EscapeAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *slot =
      dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(slot, nullptr);
  EXPECT_TRUE(analysis.isEscaped(slot));
}

TEST_F(EscapeAnalysisTest, IndirectCallEscapesStackAddress) {
  const char *source = R"(
    declare void @sink(i8*)

    define void @main() {
    entry:
      %slot = alloca i8, align 1
      %fn = select i1 true, void (i8*)* @sink, void (i8*)* @sink
      call void %fn(i8* %slot)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  EscapeAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const auto *slot =
      dyn_cast<AllocaInst>(&main_func->getEntryBlock().front());
  ASSERT_NE(slot, nullptr);
  EXPECT_TRUE(analysis.isEscaped(slot));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
