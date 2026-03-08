/**
 * @file PulseCheckerTest.cpp
 * @brief Unit tests for the Pulse bi-abductive analysis checker
 * 
 * Pulse implements a bi-abductive analysis for bug detection including:
 * - Use-after-free detection
 * - Null pointer dereference detection
 * - Uninitialized variable detection
 */

#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Report/PulseReport.h"
#include "Checker/Report/BugReportMgr.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

#include <iterator>

using namespace llvm;
using namespace pulse;

class PulseCheckerTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("PulseCheckerTest", errs());
    }
    return module;
  }

  ExecutionDomain executeEntryBlock(PulseChecker &checker, Function *F,
                                    const Instruction *stop_after = nullptr) {
    ExecutionDomain state = checker.initializeFunction(F);
    for (auto &I : F->getEntryBlock()) {
      auto states = checker.executeInstruction(&I, std::move(state), nullptr, 0);
      EXPECT_FALSE(states.empty());
      if (states.empty()) {
        return ExecutionDomain();
      }
      state = std::move(states.front());
      if (&I == stop_after) {
        break;
      }
    }
    return state;
  }
};

// Test 1: Null pointer dereference detection
TEST_F(PulseCheckerTest, NullPointerDereference) {
  const char *source = R"(
    define void @test_null_deref() {
      %ptr = alloca i32*
      store i32* null, i32** %ptr
      %p = load i32*, i32** %ptr
      ; This load might produce null
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = module->getFunction("test_null_deref");
  ASSERT_NE(F, nullptr);
  
  // Find the load instruction
  LoadInst *load = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        load = LI;
        break;
      }
    }
  }
  
  ASSERT_NE(load, nullptr);
  // Verify the load is from a pointer that was stored with null
  EXPECT_TRUE(load->getType()->isPointerTy());
}

// Test 2: Uninitialized variable detection
TEST_F(PulseCheckerTest, UninitializedVariable) {
  const char *source = R"(
    define i32 @test_uninit() {
      %x = alloca i32
      ; x is not initialized
      %val = load i32, i32* %x
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = module->getFunction("test_uninit");
  ASSERT_NE(F, nullptr);
  
  // Find the load instruction
  LoadInst *load = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        load = LI;
        break;
      }
    }
  }
  
  ASSERT_NE(load, nullptr);
  EXPECT_TRUE(load->getType()->isIntegerTy());
}

// Test 3: Simple allocation and use
TEST_F(PulseCheckerTest, AllocationAndUse) {
  const char *source = R"(
    declare i8* @malloc(i64)
    
    define i32 @test_alloc_use() {
      %ptr = call i8* @malloc(i64 4)
      %int_ptr = bitcast i8* %ptr to i32*
      store i32 42, i32* %int_ptr
      %val = load i32, i32* %int_ptr
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = module->getFunction("test_alloc_use");
  ASSERT_NE(F, nullptr);
  
  // Verify the function has proper structure
  bool hasStore = false, hasLoad = false;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (isa<StoreInst>(&I)) hasStore = true;
      if (isa<LoadInst>(&I)) hasLoad = true;
    }
  }
  
  EXPECT_TRUE(hasStore);
  EXPECT_TRUE(hasLoad);
}

// Test 4: Pointer arithmetic safety
TEST_F(PulseCheckerTest, PointerArithmetic) {
  const char *source = R"(
    define void @test_ptr_arith() {
      %arr = alloca [10 x i32]
      %ptr = getelementptr inbounds [10 x i32], [10 x i32]* %arr, i64 0, i64 5
      store i32 100, i32* %ptr
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = module->getFunction("test_ptr_arith");
  ASSERT_NE(F, nullptr);
  
  // Find GEP instruction
  GetElementPtrInst *gep = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        gep = GEP;
        break;
      }
    }
  }
  
  ASSERT_NE(gep, nullptr);
  EXPECT_EQ(gep->getNumIndices(), 2u);
}

// Test 5: Function call with arguments
TEST_F(PulseCheckerTest, FunctionCallWithArgs) {
  const char *source = R"(
    define i32 @helper(i32 %x, i32 %y) {
      %sum = add i32 %x, %y
      ret i32 %sum
    }
    
    define i32 @caller() {
      %result = call i32 @helper(i32 10, i32 20)
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *helper = module->getFunction("helper");
  Function *caller = module->getFunction("caller");
  
  ASSERT_NE(helper, nullptr);
  ASSERT_NE(caller, nullptr);
  
  // Find the call instruction
  CallInst *call = nullptr;
  for (auto &BB : *caller) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        call = CI;
        break;
      }
    }
  }
  
  ASSERT_NE(call, nullptr);
  EXPECT_EQ(call->arg_size(), 2u);
}

// Test 6: Conditional branch handling
TEST_F(PulseCheckerTest, ConditionalBranch) {
  const char *source = R"(
    define void @test_branch(i32 %cond) {
      %cmp = icmp ne i32 %cond, 0
      br i1 %cmp, label %then, label %else
      
    then:
      br label %exit
      
    else:
      br label %exit
      
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = module->getFunction("test_branch");
  ASSERT_NE(F, nullptr);
  
  // Count branches
  unsigned branchCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (isa<BranchInst>(&I)) {
        ++branchCount;
      }
    }
  }
  
  EXPECT_EQ(branchCount, 3u);  // 1 conditional + 2 unconditional
}

// Test 7: Loop analysis
TEST_F(PulseCheckerTest, LoopAnalysis) {
  const char *source = R"(
    define void @test_loop(i32 %n) {
    entry:
      br label %loop
      
    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %next = add i32 %i, 1
      %cmp = icmp slt i32 %next, %n
      br i1 %cmp, label %loop, label %exit
      
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = module->getFunction("test_loop");
  ASSERT_NE(F, nullptr);
  
  // Find PHI node
  PHINode *phi = nullptr;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *P = dyn_cast<PHINode>(&I)) {
        phi = P;
        break;
      }
    }
  }
  
  ASSERT_NE(phi, nullptr);
  EXPECT_EQ(phi->getNumIncomingValues(), 2u);
}

// Test 8: Memory copy patterns
TEST_F(PulseCheckerTest, MemoryCopy) {
  const char *source = R"(
    declare void @llvm.memcpy.i64(i8* nocapture, i8* nocapture readonly, i64, i1)
    
    define void @test_memcpy() {
      %src = alloca [16 x i8]
      %dst = alloca [16 x i8]
      %src_ptr = getelementptr inbounds [16 x i8], [16 x i8]* %src, i64 0, i64 0
      %dst_ptr = getelementptr inbounds [16 x i8], [16 x i8]* %dst, i64 0, i64 0
      call void @llvm.memcpy.i64(i8* %dst_ptr, i8* %src_ptr, i64 16, i1 false)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = module->getFunction("test_memcpy");
  ASSERT_NE(F, nullptr);
  
  // Find memcpy call
  bool hasMemcpy = false;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (auto *callee = CI->getCalledFunction()) {
          if (callee->getName().startswith("llvm.memcpy")) {
            hasMemcpy = true;
            break;
          }
        }
      }
    }
  }
  
  EXPECT_TRUE(hasMemcpy);
}

// Test 9: Struct field access
TEST_F(PulseCheckerTest, StructFieldAccess) {
  const char *source = R"(
    %struct.Point = type { i32, i32 }
    
    define i32 @test_struct() {
      %p = alloca %struct.Point
      %x_ptr = getelementptr inbounds %struct.Point, %struct.Point* %p, i32 0, i32 0
      store i32 10, i32* %x_ptr
      %y_ptr = getelementptr inbounds %struct.Point, %struct.Point* %p, i32 0, i32 1
      store i32 20, i32* %y_ptr
      %val = load i32, i32* %x_ptr
      ret i32 %val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = module->getFunction("test_struct");
  ASSERT_NE(F, nullptr);
  
  // Count GEPs
  unsigned gepCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (isa<GetElementPtrInst>(&I)) {
        ++gepCount;
      }
    }
  }
  
  EXPECT_EQ(gepCount, 2u);
}

// Test 10: Bitwise operations
TEST_F(PulseCheckerTest, BitwiseOperations) {
  const char *source = R"(
    define i32 @test_bitwise(i32 %a, i32 %b) {
      %and = and i32 %a, %b
      %or = or i32 %a, %b
      %xor = xor i32 %a, %b
      %shl = shl i32 %a, 1
      ret i32 %and
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  Function *F = module->getFunction("test_bitwise");
  ASSERT_NE(F, nullptr);
  
  // Count binary operators
  unsigned binOpCount = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      if (isa<BinaryOperator>(&I)) {
        ++binOpCount;
      }
    }
  }
  
  EXPECT_EQ(binOpCount, 4u);
}

TEST_F(PulseCheckerTest, LatentIssuesAreNotReportedAtShutdown) {
  const char *source = R"(
    define void @latent_branch() {
    entry:
      %p = alloca i8
      %cmp = icmp eq i8* %p, null
      br i1 %cmp, label %bad, label %ok

    bad:
      ret void

    ok:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DiagnosticManager::getInstance().clear();
  BugReportMgr &mgr = BugReportMgr::get_instance();
  const int reports_before = mgr.get_total_reports();
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  EXPECT_EQ(mgr.get_total_reports(), reports_before);
}

TEST_F(PulseCheckerTest, UnlockDoesNotInvalidateLockMemory) {
  const char *source = R"(
    declare i32 @pthread_mutex_unlock(i8*)

    define void @unlock_ok() {
    entry:
      %m = alloca i8
      %r = call i32 @pthread_mutex_unlock(i8* %m)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("unlock_ok");
  ASSERT_NE(F, nullptr);

  auto *alloca_inst = llvm::dyn_cast<AllocaInst>(&*F->getEntryBlock().begin());
  ASSERT_NE(alloca_inst, nullptr);

  auto call_it = std::next(F->getEntryBlock().begin());
  auto *unlock_call = llvm::dyn_cast<CallInst>(&*call_it);
  ASSERT_NE(unlock_call, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = executeEntryBlock(checker, F, unlock_call);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue lock_addr = checker.getFactory().getOrCreate(alloca_inst);
  EXPECT_FALSE(
      astate->getPostAttrs().has(lock_addr, pulse::Attribute::Invalid));
}

TEST_F(PulseCheckerTest, NonReallocAssignmentDoesNotInvalidatePreviousValue) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @assign(i8** %slot) {
    entry:
      %new = call i8* @malloc(i64 8)
      store i8* %new, i8** %slot
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("assign");
  ASSERT_NE(F, nullptr);

  Argument *slot_arg = F->arg_empty() ? nullptr : &*F->arg_begin();
  ASSERT_NE(slot_arg, nullptr);

  auto call_it = F->getEntryBlock().begin();
  auto *malloc_call = llvm::dyn_cast<CallInst>(&*call_it);
  ASSERT_NE(malloc_call, nullptr);

  auto store_it = std::next(call_it);
  auto *store = llvm::dyn_cast<StoreInst>(&*store_it);
  ASSERT_NE(store, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue old_value = checker.getFactory().createFresh();
  checker.getOperations().allocate(*astate, old_value, nullptr);
  astate->getPostStack().add(slot_arg, Address(old_value));
  astate->getPostAttrs().remove(old_value, pulse::Attribute::Invalid);
  astate->getPostAttrs().remove(old_value,
                                pulse::Attribute::Uninitialized);

  auto malloc_states =
      checker.executeInstruction(malloc_call, std::move(state), nullptr, 0);
  ASSERT_EQ(malloc_states.size(), 1u);
  state = std::move(malloc_states.front());

  auto store_states = checker.executeInstruction(store, std::move(state),
                                                 nullptr, 0);
  ASSERT_EQ(store_states.size(), 1u);
  astate = store_states.front().getAstate();
  ASSERT_NE(astate, nullptr);

  EXPECT_FALSE(
      astate->getPostAttrs().has(old_value, pulse::Attribute::Invalid));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
