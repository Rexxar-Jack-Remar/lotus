#include "SymbolicExecution/GVFGUtility.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::unittest;

namespace {

class SymbolicExecutionSpecCompatTest : public LlvmModuleTest {};

TEST_F(SymbolicExecutionSpecCompatTest,
       DeclarationCallIsNotTreatedAsDefiniteCall) {
  ASSERT_TRUE(loadModule(R"(
    declare i32 @puts(i8*)

    define i32 @caller(i8* %msg) {
    entry:
      %rv = call i32 @puts(i8* %msg)
      ret i32 %rv
    }
  )"));

  auto *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  auto *call = findInstruction<CallInst>(*caller);
  ASSERT_NE(call, nullptr);

  EXPECT_FALSE(gvfg_utility::isDefiniteCall(call));
}

TEST_F(SymbolicExecutionSpecCompatTest, DefinedCallIsTreatedAsDefiniteCall) {
  ASSERT_TRUE(loadModule(R"(
    define i32 @callee(i32 %x) {
    entry:
      %inc = add i32 %x, 1
      ret i32 %inc
    }

    define i32 @caller(i32 %x) {
    entry:
      %rv = call i32 @callee(i32 %x)
      ret i32 %rv
    }
  )"));

  auto *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  auto *call = findInstruction<CallInst>(*caller);
  ASSERT_NE(call, nullptr);

  EXPECT_TRUE(gvfg_utility::isDefiniteCall(call));
}

TEST_F(SymbolicExecutionSpecCompatTest, HeapAllocSizeRecognizesAllocatorsOnly) {
  ASSERT_TRUE(loadModule(R"(
    declare i8* @malloc(i64)
    declare i8* @memcpy(i8*, i8*, i64)

    define void @caller(i8* %dst, i8* %src) {
    entry:
      %buf = call i8* @malloc(i64 16)
      %copy = call i8* @memcpy(i8* %dst, i8* %src, i64 4)
      ret void
    }
  )"));

  auto *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  auto *malloc_call = findInstruction<CallInst>(*caller, "buf");
  ASSERT_NE(malloc_call, nullptr);
  auto malloc_sizes = gvfg_utility::getMemSpec()->getHeapAllocSize(malloc_call);
  ASSERT_EQ(malloc_sizes.size(), 1u);
  EXPECT_EQ(malloc_sizes[0], 0);

  auto *memcpy_call = findInstruction<CallInst>(*caller, "copy");
  ASSERT_NE(memcpy_call, nullptr);
  EXPECT_TRUE(gvfg_utility::getMemSpec()->getHeapAllocSize(memcpy_call).empty());
}

TEST(SymbolicExecutionSpecCompatStandaloneTest,
     KnownLibCoverageIncludesCoreLibs) {
  EXPECT_TRUE(gvfg_utility::isKnownLib("strlen"));
  EXPECT_TRUE(gvfg_utility::isKnownLib("memcpy"));
  EXPECT_TRUE(gvfg_utility::isKnownLib("recv"));
  EXPECT_FALSE(gvfg_utility::isKnownLib("definitely_not_a_known_lib"));
}

} // namespace
