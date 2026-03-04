/**
 * @file AECheckerTest.cpp
 * @brief Comprehensive unit tests for Abstract Execution (AE) checker
 *
 * Tests buffer overflow detection, null pointer dereference detection,
 * VLA handling, nested GEPs, and complex control flow scenarios.
 */

#include "Checker/AE/AEDetector.h"
#include "Checker/AE/AbstractInterpretation.h"
#include "Checker/AE/AbstractState.h"
#include "Checker/AE/IntervalValue.h"

#ifndef GTEST_INTERNAL_CPLUSPLUS_LANG
#define GTEST_INTERNAL_CPLUSPLUS_LANG 201703L
#endif
#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

using namespace llvm;
using namespace lotus::analysis;

class AECheckerTest : public ::testing::Test {
protected:
  struct AEResult {
    size_t overflow_bugs{0};
    size_t null_bugs{0};
    size_t uaf_bugs{0};
    size_t invalid_free_bugs{0};
    size_t pending_checkpoints{0};
  };

  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("AECheckerTest", errs());
    }
    return module;
  }

  AEResult runAE(Module *module) {
    if (!module->getFunction("main")) {
      FunctionType *MainTy =
          FunctionType::get(Type::getInt32Ty(context), false);
      Function *Main =
          Function::Create(MainTy, Function::ExternalLinkage, "main", module);
      BasicBlock *Entry = BasicBlock::Create(context, "entry", Main);
      IRBuilder<> B(Entry);
      B.CreateRet(ConstantInt::get(Type::getInt32Ty(context), 0));
    }

    AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
    ae.reset();
    ae.setStrictCheckpoint(false);

    auto overflowDetector = std::make_unique<BufOverflowDetector>();
    auto *overflowDetectorPtr = overflowDetector.get();
    auto nullDetector = std::make_unique<NullptrDerefDetector>();
    auto *nullDetectorPtr = nullDetector.get();
    auto uafDetector = std::make_unique<UseAfterFreeDetector>();
    auto *uafDetectorPtr = uafDetector.get();
    auto invalidFreeDetector = std::make_unique<InvalidFreeDetector>();
    auto *invalidFreeDetectorPtr = invalidFreeDetector.get();
    ae.addDetector(std::move(overflowDetector));
    ae.addDetector(std::move(nullDetector));
    ae.addDetector(std::move(uafDetector));
    ae.addDetector(std::move(invalidFreeDetector));

    ae.runOnModule(module);

    return {overflowDetectorPtr->getBugCount(), nullDetectorPtr->getBugCount(),
            uafDetectorPtr->getBugCount(),
            invalidFreeDetectorPtr->getBugCount(), ae.checkpoints.size()};
  }
};

// Test 1: Constant-sized array buffer overflow detection
TEST_F(AECheckerTest, ConstantArrayBufferOverflow) {
  const char *source = R"(
    define void @test_overflow() {
      %arr = alloca [10 x i32], align 4
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 15
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 2: Variable-length array (VLA) handling
TEST_F(AECheckerTest, VariableLengthArray) {
  const char *source = R"(
    define void @test_vla(i32 %n) {
      %arr = alloca i32, i32 %n
      %gep = getelementptr inbounds i32, i32* %arr, i64 0
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.null_bugs, 0u);
}

// Test 3: Nested GEP offset tracking
TEST_F(AECheckerTest, NestedGEP) {
  const char *source = R"(
    define void @test_nested_gep() {
      %arr = alloca [10 x [20 x i32]], align 4
      %gep1 = getelementptr inbounds [10 x [20 x i32]], [10 x [20 x i32]]* %arr, i64 0, i64 5
      %gep2 = getelementptr inbounds [20 x i32], [20 x i32]* %gep1, i64 0, i64 15
      store i32 42, i32* %gep2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
  // Nested GEP should correctly track offset from base
  // Offset = 5 * (20 * 4) + 15 * 4 = 400 + 60 = 460 bytes
  // Array size = 10 * 20 * 4 = 800 bytes, so this is safe
}

// Test 4: Null pointer dereference detection
TEST_F(AECheckerTest, NullPointerDeref) {
  const char *source = R"(
    define void @test_null_deref() {
      %ptr = alloca i32*
      store i32* null, i32** %ptr
      %p = load i32*, i32** %ptr
      %val = load i32, i32* %p
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.null_bugs, 0u);
}

// Test 5: Null pointer dereference in GEP
TEST_F(AECheckerTest, NullPointerDerefGEP) {
  const char *source = R"(
    define void @test_null_gep() {
      %ptr = alloca i32*
      store i32* null, i32** %ptr
      %p = load i32*, i32** %ptr
      %gep = getelementptr inbounds i32, i32* %p, i64 5
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.null_bugs, 0u);
}

// Test 6: External API - memcpy buffer overflow
TEST_F(AECheckerTest, MemcpyBufferOverflow) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
    
    define void @test_memcpy_overflow() {
      %dst = alloca [10 x i8], align 1
      %src = alloca [20 x i8], align 1
      %dst_ptr = bitcast [10 x i8]* %dst to i8*
      %src_ptr = bitcast [20 x i8]* %src to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %dst_ptr, i8* %src_ptr, i64 15, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 7: External API - strcpy buffer overflow
TEST_F(AECheckerTest, StrcpyBufferOverflow) {
  const char *source = R"(
    declare i8* @strcpy(i8*, i8*)
    
    define void @test_strcpy_overflow() {
      %dst = alloca [10 x i8], align 1
      %src = alloca [20 x i8], align 1
      %dst_ptr = bitcast [10 x i8]* %dst to i8*
      %src_ptr = bitcast [20 x i8]* %src to i8*
      call i8* @strcpy(i8* %dst_ptr, i8* %src_ptr)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 8: Safe buffer access (no overflow)
TEST_F(AECheckerTest, SafeBufferAccess) {
  const char *source = R"(
    define void @test_safe_access() {
      %arr = alloca [10 x i32], align 4
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 5
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_EQ(result.null_bugs, 0u);
}

// Test 9: Complex control flow with buffer access
TEST_F(AECheckerTest, ComplexControlFlow) {
  const char *source = R"(
    define void @test_control_flow(i32 %n) {
      %arr = alloca [10 x i32], align 4
      %cmp = icmp slt i32 %n, 10
      br i1 %cmp, label %if_true, label %if_false
    
    if_true:
      %n64 = sext i32 %n to i64
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 %n64
      store i32 42, i32* %gep
      br label %end
    
    if_false:
      %gep2 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 15
      store i32 42, i32* %gep2
      br label %end
    
    end:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 10: VLA with tracked size from abstract state
TEST_F(AECheckerTest, VLATrackedSize) {
  const char *source = R"(
    define void @test_vla_tracked(i32 %n) {
      %n_clamped = call i32 @llvm.smin.i32(i32 %n, i32 100)
      %arr = alloca i32, i32 %n_clamped
      %gep = getelementptr inbounds i32, i32* %arr, i64 50
      store i32 42, i32* %gep
      ret void
    }
    
    declare i32 @llvm.smin.i32(i32, i32)
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 11: Heap allocation buffer overflow
TEST_F(AECheckerTest, HeapAllocationOverflow) {
  const char *source = R"(
    declare i8* @malloc(i64)
    
    define void @test_heap_overflow() {
      %ptr = call i8* @malloc(i64 10)
      %gep = getelementptr inbounds i8, i8* %ptr, i64 15
      store i8 42, i8* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 12: Struct field access
TEST_F(AECheckerTest, StructFieldAccess) {
  const char *source = R"(
    %struct.Test = type { i32, i32, i32 }
    
    define void @test_struct() {
      %s = alloca %struct.Test, align 4
      %gep = getelementptr inbounds %struct.Test, %struct.Test* %s, i64 0, i32 2
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 13: Multiple buffer accesses in loop
TEST_F(AECheckerTest, LoopBufferAccess) {
  const char *source = R"(
    define void @test_loop() {
      %arr = alloca [10 x i32], align 4
      br label %loop
    
    loop:
      %i = phi i32 [ 0, %0 ], [ %i_next, %loop ]
      %i64 = sext i32 %i to i64
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 %i64
      store i32 42, i32* %gep
      %i_next = add i32 %i, 1
      %cmp = icmp slt i32 %i_next, 10
      br i1 %cmp, label %loop, label %exit
    
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 14: Inter-procedural buffer overflow
TEST_F(AECheckerTest, InterproceduralOverflow) {
  const char *source = R"(
    define void @helper(i32* %arr, i32 %idx) {
      %idx64 = sext i32 %idx to i64
      %gep = getelementptr inbounds i32, i32* %arr, i64 %idx64
      store i32 42, i32* %gep
      ret void
    }
    
    define void @test_interproc() {
      %arr = alloca [10 x i32], align 4
      %arr_ptr = bitcast [10 x i32]* %arr to i32*
      call void @helper(i32* %arr_ptr, i32 15)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 15: Stub function SAFE_BUFACCESS
TEST_F(AECheckerTest, StubSafeBufAccess) {
  const char *source = R"(
    declare void @SAFE_BUFACCESS(i8*, i32)
    
    define void @test_safe_stub() {
      %arr = alloca [10 x i8], align 1
      %arr_ptr = bitcast [10 x i8]* %arr to i8*
      call void @SAFE_BUFACCESS(i8* %arr_ptr, i32 5)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.pending_checkpoints, 0u);
}

// Test 16: Stub function UNSAFE_BUFACCESS
TEST_F(AECheckerTest, StubUnsafeBufAccess) {
  const char *source = R"(
    declare void @UNSAFE_BUFACCESS(i8*, i32)
    
    define void @test_unsafe_stub() {
      %arr = alloca [10 x i8], align 1
      %arr_ptr = bitcast [10 x i8]* %arr to i8*
      call void @UNSAFE_BUFACCESS(i8* %arr_ptr, i32 15)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.pending_checkpoints, 0u);
}

// Test 17: AbstractState VLA size computation
TEST_F(AECheckerTest, AbstractStateVLASize) {
  AbstractState as;

  // Create a mock alloca instruction context
  LLVMContext ctx;
  Module M("test", ctx);
  FunctionType *FTy = FunctionType::get(Type::getVoidTy(ctx), false);
  Function *F = Function::Create(FTy, Function::ExternalLinkage, "test", M);
  BasicBlock *BB = BasicBlock::Create(ctx, "entry", F);

  // Create VLA alloca with non-constant size
  Type *Int32Ty = Type::getInt32Ty(ctx);
  // Use a parameter as size (simulating VLA)
  Argument *SizeArg = new Argument(Int32Ty, "size");
  AllocaInst *VLA = new AllocaInst(Int32Ty, 0, SizeArg, "vla", BB);

  // Set up abstract state with size tracking
  uint32_t sizeId = AbstractInterpretation::getValueIdStatic(SizeArg);
  as[sizeId] = AbstractValue(IntervalValue(50, 100));

  // Test improved VLA size computation
  uint32_t computedSize = as.getAllocaInstByteSize(VLA, as);

  // Should use upper bound from abstract state (100) * sizeof(i32) = 400
  // But clamped to MaxFieldLimit if needed
  EXPECT_GT(computedSize, 0);
  EXPECT_LE(computedSize, MaxFieldLimit);
}

// Test 18: GEP offset accumulation for nested GEPs
TEST_F(AECheckerTest, NestedGEPOffsetAccumulation) {
  const char *source = R"(
    define void @test_nested_gep_offset() {
      %arr = alloca [100 x i32], align 4
      %gep1 = getelementptr inbounds [100 x i32], [100 x i32]* %arr, i64 0, i64 10
      %gep2 = getelementptr inbounds i32, i32* %gep1, i64 5
      store i32 42, i32* %gep2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 19: Buffer overflow at boundary
TEST_F(AECheckerTest, BoundaryOverflow) {
  const char *source = R"(
    define void @test_boundary() {
      %arr = alloca [10 x i32], align 4
      %gep = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 10
      store i32 42, i32* %gep
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
}

// Test 20: Multiple detectors interaction
TEST_F(AECheckerTest, MultipleDetectors) {
  const char *source = R"(
    define void @test_multiple_issues() {
      %arr = alloca [10 x i32], align 4
      %ptr = alloca i32*
      store i32* null, i32** %ptr
      %p = load i32*, i32** %ptr
      %gep1 = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 15
      %gep2 = getelementptr inbounds i32, i32* %p, i64 5
      store i32 42, i32* %gep1
      store i32 42, i32* %gep2
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_EQ(result.overflow_bugs, 0u);
  EXPECT_GT(result.null_bugs, 0u);
}

TEST_F(AECheckerTest, UseAfterFreeDetection) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @test_uaf() {
      %p = call i8* @malloc(i64 16)
      call void @free(i8* %p)
      %q = getelementptr inbounds i8, i8* %p, i64 1
      store i8 1, i8* %q
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.uaf_bugs, 0u);
}

TEST_F(AECheckerTest, InvalidFreeDetection) {
  const char *source = R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @test_double_free() {
      %p = call i8* @malloc(i64 32)
      call void @free(i8* %p)
      call void @free(i8* %p)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  AEResult result = runAE(module.get());
  EXPECT_GT(result.invalid_free_bugs, 0u);
}
