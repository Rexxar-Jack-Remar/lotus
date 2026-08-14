#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "Utils/LLVM/CallUtils.h"

#include <gtest/gtest.h>

namespace {

TEST(LLVMCallUtilsTest, ResolvesDirectAndAliasedCallees) {
  llvm::LLVMContext Context;
  llvm::Module Module("call-utils", Context);
  llvm::IRBuilder<> Builder(Context);

  auto *FunctionType = llvm::FunctionType::get(Builder.getInt32Ty(),
                                               {Builder.getInt32Ty()}, false);
  auto *Target = llvm::Function::Create(
      FunctionType, llvm::GlobalValue::ExternalLinkage, "target", Module);
  auto *Alias = llvm::GlobalAlias::create(FunctionType, 0,
                                          llvm::GlobalValue::ExternalLinkage,
                                          "target_alias", Target, &Module);

  auto *CallerType = llvm::FunctionType::get(Builder.getVoidTy(), false);
  auto *Caller = llvm::Function::Create(
      CallerType, llvm::GlobalValue::ExternalLinkage, "caller", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Caller);
  Builder.SetInsertPoint(Entry);

  auto *DirectCall = Builder.CreateCall(Target, {Builder.getInt32(1)});
  auto *AliasCall =
      Builder.CreateCall(FunctionType, Alias, {Builder.getInt32(2)});
  Builder.CreateRetVoid();

  EXPECT_EQ(lotus::llvm_utils::getDirectCallee(DirectCall), Target);
  EXPECT_EQ(lotus::llvm_utils::getDirectCallee(AliasCall), Target);
}

TEST(LLVMCallUtilsTest, DoesNotResolveIndirectCallee) {
  llvm::LLVMContext Context;
  llvm::Module Module("indirect-call-utils", Context);
  llvm::IRBuilder<> Builder(Context);

  auto *CalleeType = llvm::FunctionType::get(Builder.getInt32Ty(),
                                             {Builder.getInt32Ty()}, false);
  auto *CallerType = llvm::FunctionType::get(
      Builder.getVoidTy(), {CalleeType->getPointerTo()}, false);
  auto *Caller = llvm::Function::Create(
      CallerType, llvm::GlobalValue::ExternalLinkage, "caller", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Caller);
  Builder.SetInsertPoint(Entry);

  auto *IndirectCall =
      Builder.CreateCall(CalleeType, Caller->getArg(0), {Builder.getInt32(1)});
  Builder.CreateRetVoid();

  EXPECT_EQ(lotus::llvm_utils::getDirectCallee(IndirectCall), nullptr);
}

TEST(LLVMCallUtilsTest, FindsNormalCallAndInvokeContinuations) {
  llvm::LLVMContext Context;
  llvm::Module Module("call-continuations", Context);
  llvm::IRBuilder<> Builder(Context);

  auto *CalleeType = llvm::FunctionType::get(Builder.getVoidTy(), false);
  auto *Callee = llvm::Function::Create(
      CalleeType, llvm::GlobalValue::ExternalLinkage, "callee", Module);

  auto *CallFunction = llvm::Function::Create(
      CalleeType, llvm::GlobalValue::ExternalLinkage, "call_function", Module);
  auto *CallEntry = llvm::BasicBlock::Create(Context, "entry", CallFunction);
  Builder.SetInsertPoint(CallEntry);
  auto *Call = Builder.CreateCall(Callee);
  auto *ReturnAfterCall = Builder.CreateRetVoid();

  auto CallContinuations = lotus::llvm_utils::getNormalCallContinuations(Call);
  ASSERT_EQ(CallContinuations.size(), 1U);
  EXPECT_EQ(CallContinuations.front(), ReturnAfterCall);

  auto *InvokeFunction =
      llvm::Function::Create(CalleeType, llvm::GlobalValue::ExternalLinkage,
                             "invoke_function", Module);
  auto *InvokeEntry =
      llvm::BasicBlock::Create(Context, "entry", InvokeFunction);
  auto *Normal = llvm::BasicBlock::Create(Context, "normal", InvokeFunction);
  auto *Unwind = llvm::BasicBlock::Create(Context, "unwind", InvokeFunction);

  Builder.SetInsertPoint(InvokeEntry);
  auto *Invoke = Builder.CreateInvoke(Callee, Normal, Unwind);
  Builder.SetInsertPoint(Normal);
  auto *NormalReturn = Builder.CreateRetVoid();
  Builder.SetInsertPoint(Unwind);
  auto *UnwindTerminator = Builder.CreateUnreachable();

  auto InvokeContinuations =
      lotus::llvm_utils::getNormalCallContinuations(Invoke);
  ASSERT_EQ(InvokeContinuations.size(), 1U);
  EXPECT_EQ(InvokeContinuations.front(), NormalReturn);
  EXPECT_NE(InvokeContinuations.front(), UnwindTerminator);
}

} // namespace
