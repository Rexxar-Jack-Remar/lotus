#include "Analysis/DebugInfo/DebugInfoAnalysis.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <gtest/gtest.h>

namespace {

std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext &context,
                                          const char *source) {
  llvm::SMDiagnostic err;
  auto module = llvm::parseAssemblyString(source, err, context);
  if (!module) {
    err.print("DebugInfoAnalysisTest", llvm::errs());
  }
  return module;
}

const llvm::Instruction *findInstruction(const llvm::Function *function,
                                         llvm::StringRef name) {
  for (const auto &bb : *function) {
    for (const auto &inst : bb) {
      if (inst.getName() == name) {
        return &inst;
      }
    }
  }
  return nullptr;
}

TEST(DebugInfoAnalysisTest, FallsBackToIrNamesWithoutDebugMetadata) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @example() {
    entry:
      %value = add i32 1, 2
      ret i32 %value
    }
  )");
  ASSERT_NE(module, nullptr);

  const auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  const auto *value = findInstruction(function, "value");
  ASSERT_NE(value, nullptr);

  DebugInfoAnalysis analysis;
  EXPECT_EQ(analysis.getFunctionName(value), "example");
  EXPECT_EQ(analysis.getVariableName(value), "value");
  EXPECT_EQ(analysis.getTypeName(value), "i32");
  EXPECT_EQ(analysis.getSourceLocation(value), "unknown:0");
}

TEST(DebugInfoAnalysisTest, UsesInstructionNameWhenNoDebugVariableExists) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    declare i32 @producer()

    define i32 @example() {
    entry:
      %tmp = call i32 @producer()
      ret i32 %tmp
    }
  )");
  ASSERT_NE(module, nullptr);

  const auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  const auto *call = findInstruction(function, "tmp");
  ASSERT_NE(call, nullptr);

  DebugInfoAnalysis analysis;
  EXPECT_EQ(analysis.getVariableName(call), "tmp");
  EXPECT_EQ(analysis.getFunctionName(call), "example");
}

} // namespace
