/**
 * @file MHPAnalysisTest.cpp
 * @brief Simplified unit tests for MHP Analysis
 */

#include "Analysis/Concurrency/MHP/MHPAnalysis.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace mhp;

static const Instruction *findInstructionByName(const Function &func,
                                                StringRef name) {
  for (const auto &bb : func) {
    for (const auto &inst : bb) {
      if (inst.getName() == name) {
        return &inst;
      }
    }
  }
  return nullptr;
}

class MHPAnalysisTest : public ::testing::Test {
protected:
  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("MHPAnalysisTest", errs());
    }
    return module;
  }
};

// Test 1: Simple main function
TEST_F(MHPAnalysisTest, SimpleMain) {
  const char *source = R"(
    define i32 @main() {
      %x = add i32 1, 2
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  EXPECT_NO_THROW(mhp.analyze());

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_threads, 0);
}

// Test 2: Thread creation
TEST_F(MHPAnalysisTest, ThreadCreation) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    
    define i8* @worker(i8* %arg) {
      ret i8* null
    }
    
    define i32 @main() {
      %tid = alloca i8
      %ret = call i32 @pthread_create(i8* %tid, i8* null, 
                                       i8* (i8*)* @worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_forks, 1);
}

// Test 3: Lock operations
TEST_F(MHPAnalysisTest, LockOperations) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)
    
    @lock = global i8 0
    
    define i32 @main() {
      %l = call i32 @pthread_mutex_lock(i8* @lock)
      %x = add i32 1, 2
      %u = call i32 @pthread_mutex_unlock(i8* @lock)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_locks, 1);
  EXPECT_GE(stats.num_unlocks, 1);
}

TEST_F(MHPAnalysisTest, JoinStatistics) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
      ret i8* null
    }

    define i32 @main() {
      %tid = alloca i8
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %join = call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_forks, 1);
  EXPECT_GE(stats.num_joins, 1);
}

TEST_F(MHPAnalysisTest, ThreadFlowGraphNodes) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define i8* @worker(i8* %arg) {
      %l = call i32 @pthread_mutex_lock(i8* @lock)
      %u = call i32 @pthread_mutex_unlock(i8* @lock)
      ret i8* null
    }

    define i32 @main() {
      %tid = alloca i8
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %join = call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const ThreadFlowGraph &tfg = mhp.getThreadFlowGraph();
  auto forkNodes = tfg.getNodesOfType(SyncNodeType::THREAD_FORK);
  auto joinNodes = tfg.getNodesOfType(SyncNodeType::THREAD_JOIN);
  auto lockNodes = tfg.getNodesOfType(SyncNodeType::LOCK_ACQUIRE);
  auto unlockNodes = tfg.getNodesOfType(SyncNodeType::LOCK_RELEASE);

  EXPECT_GE(forkNodes.size(), 1u);
  EXPECT_GE(joinNodes.size(), 1u);
  EXPECT_GE(lockNodes.size(), 1u);
  EXPECT_GE(unlockNodes.size(), 1u);
}

TEST_F(MHPAnalysisTest, ForkJoinOrdering) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
      %w1 = add i32 40, 2
      %w2 = add i32 %w1, 1
      ret i8* null
    }

    define i32 @main() {
      %tid = alloca i8
      %pre = add i32 1, 2
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %mid = add i32 3, 4
      %join = call i32 @pthread_join(i8* %tid, i8* null)
      %post = add i32 5, 6
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *pre = findInstructionByName(*main_func, "pre");
  const Instruction *mid = findInstructionByName(*main_func, "mid");
  const Instruction *post = findInstructionByName(*main_func, "post");
  const Instruction *w1 = findInstructionByName(*worker_func, "w1");
  ASSERT_NE(pre, nullptr);
  ASSERT_NE(mid, nullptr);
  ASSERT_NE(post, nullptr);
  ASSERT_NE(w1, nullptr);

  EXPECT_TRUE(mhp.mustBeSequential(pre, w1));
  EXPECT_TRUE(mhp.mayHappenInParallel(mid, w1));
  EXPECT_TRUE(mhp.mustBeSequential(post, w1));
}

TEST_F(MHPAnalysisTest, LoopForkCreatesMultiInstanceThread) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
      %w1 = add i32 10, 20
      %w2 = add i32 %w1, 1
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      br label %loop

    loop:
      %i = phi i32 [0, %entry], [%inc, %loop]
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %inc = add i32 %i, 1
      %cond = icmp slt i32 %inc, 2
      br i1 %cond, label %loop, label %exit

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(worker_func, nullptr);

  const Instruction *w1 = findInstructionByName(*worker_func, "w1");
  const Instruction *w2 = findInstructionByName(*worker_func, "w2");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);

  EXPECT_TRUE(mhp.mayHappenInParallel(w1, w2));
}

TEST_F(MHPAnalysisTest, MutexSerializesCriticalSections) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define i8* @worker(i8* %arg) {
      %wl = call i32 @pthread_mutex_lock(i8* @lock)
      %w_in = add i32 7, 8
      %wu = call i32 @pthread_mutex_unlock(i8* @lock)
      ret i8* null
    }

    define i32 @main() {
      %tid = alloca i8
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %ml = call i32 @pthread_mutex_lock(i8* @lock)
      %m_in = add i32 1, 2
      %mu = call i32 @pthread_mutex_unlock(i8* @lock)
      %join = call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.enableLockSetAnalysis();
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *m_in = findInstructionByName(*main_func, "m_in");
  const Instruction *w_in = findInstructionByName(*worker_func, "w_in");
  ASSERT_NE(m_in, nullptr);
  ASSERT_NE(w_in, nullptr);

  // With lockset enabled, instructions inside the same mutex critical section
  // should be considered non-parallel.
  auto *lockset = mhp.getLockSetAnalysis();
  ASSERT_NE(lockset, nullptr);
  auto stats = lockset->getStatistics();
  EXPECT_EQ(stats.num_locks, 1u);
  EXPECT_GE(stats.num_acquires, 2u);
  EXPECT_GE(stats.num_releases, 2u);
  EXPECT_FALSE(mhp.mayHappenInParallel(m_in, w_in));
}

TEST_F(MHPAnalysisTest, BarrierOrdersPreAndPostRegions) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_barrier_wait(i8*)

    @bar = global i8 0
    @shared = global i32 0

    define i8* @writer(i8* %arg) {
    entry:
      store i32 42, i32* @shared, align 4
      %bw = call i32 @pthread_barrier_wait(i8* @bar)
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %bw = call i32 @pthread_barrier_wait(i8* @bar)
      %val = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *writer_func = module->getFunction("writer");
  const Function *reader_func = module->getFunction("reader");
  ASSERT_NE(writer_func, nullptr);
  ASSERT_NE(reader_func, nullptr);

  const Instruction *store_shared = &writer_func->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*reader_func, "val");
  ASSERT_NE(load_shared, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  EXPECT_TRUE(mhp.mustPrecede(store_shared, load_shared));
  EXPECT_FALSE(mhp.mayHappenInParallel(store_shared, load_shared));
}

TEST_F(MHPAnalysisTest, JoinTargetThroughPhiResolves) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      br i1 %cond, label %left, label %right

    left:
      br label %join

    right:
      br label %join

    join:
      %phi_tid = phi i8* [ %tid, %left ], [ %tid, %right ]
      %joined = call i32 @pthread_join(i8* %phi_tid, i8* null)
      %post = add i32 3, 4
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *worker_inst = findInstructionByName(*worker_func, "w");
  const Instruction *post = findInstructionByName(*main_func, "post");
  ASSERT_NE(worker_inst, nullptr);
  ASSERT_NE(post, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  EXPECT_TRUE(mhp.mustBeSequential(worker_inst, post));
}

TEST_F(MHPAnalysisTest, RegionPartitionDoesNotOverlapAcrossBranchMerge) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)

    @lock = global i8 0

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else

    then:
      %lock_then = call i32 @pthread_mutex_lock(i8* @lock)
      br label %merge

    else:
      %plain = add i32 1, 2
      br label %merge

    merge:
      %merge_val = phi i32 [ 1, %then ], [ %plain, %else ]
      ret i32 %merge_val
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  size_t inst_count = 0;
  for (const BasicBlock &bb : *main_func) {
    for (const Instruction &inst : bb) {
      (void)inst;
      ++inst_count;
    }
  }

  size_t covered = 0;
  for (const auto &region : mhp.getThreadRegionAnalysis().getAllRegions()) {
    if (region->thread_id != 0) {
      continue;
    }
    covered += region->instructions.size();
  }

  EXPECT_EQ(covered, inst_count);
}

TEST_F(MHPAnalysisTest, HelperCalledBeforeAndAfterForkIsNotGloballyPrefork) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i32 @helper() {
    entry:
      %h = add i32 1, 2
      ret i32 %h
    }

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %pre = call i32 @helper()
      %tid = alloca i8
      %fork = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %post = call i32 @helper()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *helper_func = module->getFunction("helper");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(helper_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  const Instruction *helper_inst = findInstructionByName(*helper_func, "h");
  const Instruction *worker_inst = findInstructionByName(*worker_func, "w");
  ASSERT_NE(helper_inst, nullptr);
  ASSERT_NE(worker_inst, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  EXPECT_EQ(mhp.getThreadFlowGraph().getNodes(helper_inst).size(), 2u);
  EXPECT_FALSE(mhp.mustPrecede(helper_inst, worker_inst));
  EXPECT_TRUE(mhp.mayHappenInParallel(helper_inst, worker_inst));
}

// Main function for tests
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
