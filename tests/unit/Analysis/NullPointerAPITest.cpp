#include "Analysis/NullPointer/API.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
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
    err.print("NullPointerAPITest", llvm::errs());
  }
  return module;
}

llvm::Instruction *findInstructionByName(llvm::Function *function,
                                         llvm::StringRef name) {
  for (auto &bb : *function) {
    for (auto &inst : bb) {
      if (inst.getName() == name) {
        return &inst;
      }
    }
  }
  return nullptr;
}

TEST(NullPointerAPITest, DistinguishesHeapAndStackAllocations) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define i32 @example() {
    entry:
      %stack = alloca i32, align 4
      %heap = call i8* @malloc(i64 8)
      store i32 0, i32* %stack, align 4
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  auto *stack_alloc = findInstructionByName(function, "stack");
  auto *heap_alloc = findInstructionByName(function, "heap");
  ASSERT_NE(stack_alloc, nullptr);
  ASSERT_NE(heap_alloc, nullptr);

  EXPECT_TRUE(API::isStackAllocate(stack_alloc));
  EXPECT_FALSE(API::isHeapAllocate(stack_alloc));
  EXPECT_TRUE(API::isMemoryAllocate(stack_alloc));

  EXPECT_TRUE(API::isHeapAllocate(heap_alloc));
  EXPECT_FALSE(API::isStackAllocate(heap_alloc));
  EXPECT_TRUE(API::isMemoryAllocate(heap_alloc));
}

TEST(NullPointerAPITest, IgnoresNonAllocationCalls) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    declare void @free(i8*)

    define void @example(i8* %ptr) {
    entry:
      call void @free(i8* %ptr)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *function = module->getFunction("example");
  ASSERT_NE(function, nullptr);
  auto *call = llvm::dyn_cast<llvm::CallInst>(&function->getEntryBlock().front());
  ASSERT_NE(call, nullptr);

  EXPECT_FALSE(API::isHeapAllocate(call));
  EXPECT_FALSE(API::isStackAllocate(call));
  EXPECT_FALSE(API::isMemoryAllocate(call));
}

} // namespace
