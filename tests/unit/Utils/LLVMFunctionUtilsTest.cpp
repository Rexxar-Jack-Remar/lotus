#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "Utils/LLVM/FunctionUtils.h"

#include <gtest/gtest.h>

namespace {

TEST(LLVMFunctionUtilsTest, FindsEntryInstructionOfDefinedFunction) {
  llvm::LLVMContext Context;
  llvm::Module Module("function-utils", Context);
  llvm::IRBuilder<> Builder(Context);

  auto *FunctionType = llvm::FunctionType::get(Builder.getVoidTy(), false);
  auto *Function = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::ExternalLinkage, "defined", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  Builder.SetInsertPoint(Entry);
  auto *First = Builder.CreateAlloca(Builder.getInt32Ty());
  auto *Return = Builder.CreateRetVoid();

  EXPECT_EQ(lotus::llvm_utils::getFunctionEntryInstruction(Function), First);
  EXPECT_TRUE(lotus::llvm_utils::isFunctionEntryInstruction(First));
  EXPECT_FALSE(lotus::llvm_utils::isFunctionEntryInstruction(Return));
}

TEST(LLVMFunctionUtilsTest, RejectsDeclarationsAndNullInstructions) {
  llvm::LLVMContext Context;
  llvm::Module Module("function-utils-declaration", Context);
  llvm::IRBuilder<> Builder(Context);

  auto *FunctionType = llvm::FunctionType::get(Builder.getVoidTy(), false);
  auto *Declaration = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::ExternalLinkage, "declaration", Module);

  EXPECT_EQ(lotus::llvm_utils::getFunctionEntryInstruction(Declaration),
            nullptr);
  EXPECT_FALSE(lotus::llvm_utils::isFunctionEntryInstruction(nullptr));
}

} // namespace
