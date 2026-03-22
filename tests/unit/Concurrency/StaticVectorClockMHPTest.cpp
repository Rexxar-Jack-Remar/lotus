#include "Analysis/Concurrency/MHP/StaticVectorClockMHP.h"

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

class StaticVectorClockMHPTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("StaticVectorClockMHPTest", errs());
    }
    return module;
  }
};

TEST_F(StaticVectorClockMHPTest, LoopForkDoesNotAutoSelfParallelizeWorkerBody) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
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
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      %inc = add i32 %i, 1
      %cond = icmp slt i32 %inc, 2
      br i1 %cond, label %loop, label %exit

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  StaticVectorClockMHP svc(*module);
  svc.analyze();

  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(worker_func, nullptr);
  const Instruction *w1 = findInstructionByName(*worker_func, "w1");
  const Instruction *w2 = findInstructionByName(*worker_func, "w2");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);
  EXPECT_FALSE(svc.mayHappenInParallel(w1, w2));
}

TEST_F(StaticVectorClockMHPTest,
       LoopCreateJoinDoesNotAutoSelfParallelizeWorkerBody) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %a = add i32 1, 2
      %b = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      br label %loop

    loop:
      %i = phi i32 [0, %entry], [%next, %loop]
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call i32 @pthread_join(i8* %tid, i8* null)
      %next = add i32 %i, 1
      %cond = icmp slt i32 %next, 2
      br i1 %cond, label %loop, label %exit

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  StaticVectorClockMHP svc(*module);
  svc.analyze();

  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(worker_func, nullptr);
  const Instruction *a = findInstructionByName(*worker_func, "a");
  const Instruction *b = findInstructionByName(*worker_func, "b");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);

  EXPECT_FALSE(svc.mayHappenInParallel(a, b));
}

TEST_F(StaticVectorClockMHPTest, BarrierOrdersPostBarrierContinuation) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_barrier_wait(i8*)

    @bar = global i8 0
    @shared = global i32 0

    define i8* @writer(i8* %arg) {
    entry:
      store i32 42, i32* @shared, align 4
      call i32 @pthread_barrier_wait(i8* @bar)
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      call i32 @pthread_barrier_wait(i8* @bar)
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

  StaticVectorClockMHP svc(*module);
  svc.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);
  EXPECT_TRUE(svc.happensBefore(store_shared, load_shared));
}

TEST_F(StaticVectorClockMHPTest, SplitPhaseBarrierOrdersPostBarrierContinuation) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_barrier_arriveEv(i8*)
    declare void @std_barrier_waitEv(i8*)

    @barrier = global i8 0
    @shared = global i32 0

    define i8* @writer(i8* %arg) {
    entry:
      store i32 23, i32* @shared, align 4
      call void @std_barrier_arriveEv(i8* @barrier)
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      call void @std_barrier_waitEv(i8* @barrier)
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

  StaticVectorClockMHP svc(*module);
  svc.analyze();

  const Instruction *store_shared =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);
  EXPECT_TRUE(svc.happensBefore(store_shared, load_shared));
  EXPECT_FALSE(svc.mayHappenInParallel(store_shared, load_shared));
}

TEST_F(StaticVectorClockMHPTest, LatchArriveAndWaitCreatesBarrierWaitNode) {
  const char *source = R"(
    declare void @fake_latch_arrive_and_wait(i8*)

    @latch = global i8 0

    define i32 @main() {
    entry:
      call void @fake_latch_arrive_and_wait(i8* @latch)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  StaticVectorClockMHP svc(*module);
  svc.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  const Instruction *call = nullptr;
  for (const Instruction &inst : main_func->getEntryBlock()) {
    if (isa<CallBase>(&inst)) {
      call = &inst;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  const ThreadFlowGraph &tfg = svc.getThreadFlowGraph();
  auto nodes = tfg.getNodes(call);
  ASSERT_EQ(nodes.size(), 1u);
  EXPECT_EQ(nodes.front()->getType(), SyncNodeType::BARRIER_WAIT);
}

TEST_F(StaticVectorClockMHPTest, StdThreadJoinCreatesJoinLikeHB) {
  const char *source = R"(
    declare void @_ZNSt6threadC1EPFvPvES0_(i8*, i8* (i8*)*, i8*)
    declare void @_ZNSt6thread4joinEv(i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define void @main() {
    entry:
      %thr = alloca i8
      call void @_ZNSt6threadC1EPFvPvES0_(i8* %thr, i8* (i8*)* @worker, i8* null)
      call void @_ZNSt6thread4joinEv(i8* %thr)
      %post = add i32 3, 4
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  StaticVectorClockMHP svc(*module);
  svc.analyze();

  const Instruction *worker_inst = findInstructionByName(*module->getFunction("worker"), "w");
  const Instruction *post = findInstructionByName(*module->getFunction("main"), "post");
  ASSERT_NE(worker_inst, nullptr);
  ASSERT_NE(post, nullptr);
  EXPECT_FALSE(svc.mayHappenInParallel(worker_inst, post));
}

TEST_F(StaticVectorClockMHPTest, JoinTargetThroughLoadCreatesJoinLikeHB) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      %slot = alloca i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      store i8* %tid, i8** %slot
      %join_tid = load i8*, i8** %slot
      call i32 @pthread_join(i8* %join_tid, i8* null)
      %post = add i32 3, 4
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  StaticVectorClockMHP svc(*module);
  svc.analyze();

  const Instruction *worker_inst =
      findInstructionByName(*module->getFunction("worker"), "w");
  const Instruction *post =
      findInstructionByName(*module->getFunction("main"), "post");
  ASSERT_NE(worker_inst, nullptr);
  ASSERT_NE(post, nullptr);
  EXPECT_FALSE(svc.mayHappenInParallel(worker_inst, post));
}

TEST_F(StaticVectorClockMHPTest, ForeignJoinHandleDoesNotCreateJoinLikeHB) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define i32 @main(i8* %foreign_tid) {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call i32 @pthread_join(i8* %foreign_tid, i8* null)
      %post = add i32 3, 4
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  StaticVectorClockMHP svc(*module);
  svc.analyze();

  const Instruction *worker_inst =
      findInstructionByName(*module->getFunction("worker"), "w");
  const Instruction *post =
      findInstructionByName(*module->getFunction("main"), "post");
  ASSERT_NE(worker_inst, nullptr);
  ASSERT_NE(post, nullptr);
  EXPECT_TRUE(svc.mayHappenInParallel(worker_inst, post));
}

TEST_F(StaticVectorClockMHPTest, ReusedThreadHandleStorageKeepsJoinAmbiguous) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      %w1 = add i32 1, 2
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      %w2 = add i32 3, 4
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid = alloca i8
      br i1 %cond, label %left, label %right

    left:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker1, i8* null)
      br label %join

    right:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker2, i8* null)
      br label %join

    join:
      call i32 @pthread_join(i8* %tid, i8* null)
      %post = add i32 5, 6
      ret i32 %post
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  StaticVectorClockMHP svc(*module);
  svc.analyze();

  const Instruction *w1 =
      findInstructionByName(*module->getFunction("worker1"), "w1");
  const Instruction *w2 =
      findInstructionByName(*module->getFunction("worker2"), "w2");
  const Instruction *post =
      findInstructionByName(*module->getFunction("main"), "post");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);
  ASSERT_NE(post, nullptr);

  EXPECT_TRUE(svc.mayHappenInParallel(w1, post));
  EXPECT_TRUE(svc.mayHappenInParallel(w2, post));
}

TEST_F(StaticVectorClockMHPTest, ParallelInstructionsMatchesQueries) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
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
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      %inc = add i32 %i, 1
      %cond = icmp slt i32 %inc, 2
      br i1 %cond, label %loop, label %exit

    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  StaticVectorClockMHP svc(*module);
  svc.analyze();

  const Instruction *w1 = findInstructionByName(*module->getFunction("worker"), "w1");
  const Instruction *w2 = findInstructionByName(*module->getFunction("worker"), "w2");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);

  InstructionSet parallel = svc.getParallelInstructions(w1);
  EXPECT_EQ(parallel.count(w2), 0u);
  EXPECT_FALSE(svc.mayHappenInParallel(w1, w2));
}

TEST_F(StaticVectorClockMHPTest,
       UnresolvedIndirectCallEnablesConservativeForkFallback) {
  const char *source = R"(
    @hook = external global void ()*

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %w = add i32 1, 2
      ret i8* null
    }

    define void @fork_helper() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %fn = load void ()*, void ()** @hook
      call void %fn()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  StaticVectorClockMHP svc(*module);
  svc.analyze();

  const Function *worker = module->getFunction("worker");
  ASSERT_NE(worker, nullptr);
  const Instruction *worker_inst = findInstructionByName(*worker, "w");
  ASSERT_NE(worker_inst, nullptr);

  EXPECT_EQ(svc.getThreadID(worker_inst), std::numeric_limits<ThreadID>::max());
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
