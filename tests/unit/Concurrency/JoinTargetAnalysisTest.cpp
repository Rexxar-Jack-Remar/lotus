#include "Analysis/Concurrency/JoinTarget/JoinTargetAnalysis.h"

#include "LLVMHelpers.h"

using namespace llvm;
using namespace mhp;

class JoinTargetAnalysisTest : public lotus::unittest::LlvmModuleTest {};

TEST_F(JoinTargetAnalysisTest, ResolvesJoinThroughPhi) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
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
      %join_tid = phi i8* [ %tid, %left ], [ %tid, %right ]
      %joined = call i32 @pthread_join(i8* %join_tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *join_inst = nullptr;
  for (const Instruction &inst : instructions(main_func)) {
    if (const auto *cb = dyn_cast<CallBase>(&inst)) {
      if (cb->getCalledFunction() &&
          cb->getCalledFunction()->getName() == "pthread_join") {
        join_inst = &inst;
        break;
      }
    }
  }

  ASSERT_NE(join_inst, nullptr);
  EXPECT_TRUE(analysis.isUnambiguousJoin(join_inst));
  auto forks = analysis.getPossibleJoinedForks(join_inst);
  EXPECT_EQ(forks.size(), 1u);
}

TEST_F(JoinTargetAnalysisTest, LeavesAmbiguousJoinAmbiguous) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      ret i8* null
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      %join_tid = select i1 %cond, i8* %tid1, i8* %tid2
      %joined = call i32 @pthread_join(i8* %join_tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *join_inst = nullptr;
  for (const Instruction &inst : instructions(main_func)) {
    if (const auto *cb = dyn_cast<CallBase>(&inst)) {
      if (cb->getCalledFunction() &&
          cb->getCalledFunction()->getName() == "pthread_join") {
        join_inst = &inst;
        break;
      }
    }
  }

  ASSERT_NE(join_inst, nullptr);
  EXPECT_FALSE(analysis.isUnambiguousJoin(join_inst));
  auto forks = analysis.getPossibleJoinedForks(join_inst);
  EXPECT_EQ(forks.size(), 2u);
}

TEST_F(JoinTargetAnalysisTest, ResolvesJoinThroughLoadAndBitcastToSingleFork) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      %slot = alloca i8*
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      store i8* %tid, i8** %slot
      %loaded = load i8*, i8** %slot
      %bc = bitcast i8* %loaded to i8*
      %joined = call i32 @pthread_join(i8* %bc, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *join_inst = nullptr;
  for (const Instruction &inst : instructions(main_func)) {
    if (const auto *cb = dyn_cast<CallBase>(&inst)) {
      if (cb->getCalledFunction() &&
          cb->getCalledFunction()->getName() == "pthread_join") {
        join_inst = &inst;
        break;
      }
    }
  }

  ASSERT_NE(join_inst, nullptr);
  EXPECT_TRUE(analysis.isUnambiguousJoin(join_inst));
  EXPECT_EQ(analysis.getPossibleJoinedForks(join_inst).size(), 1u);
}

TEST_F(JoinTargetAnalysisTest, ExactRootsBeatAliasFallbackAmbiguity) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      %join_tid = bitcast i8* %tid1 to i8*
      %joined = call i32 @pthread_join(i8* %join_tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *join_inst = nullptr;
  for (const Instruction &inst : instructions(main_func)) {
    if (const auto *cb = dyn_cast<CallBase>(&inst)) {
      if (cb->getCalledFunction() &&
          cb->getCalledFunction()->getName() == "pthread_join") {
        join_inst = &inst;
        break;
      }
    }
  }

  ASSERT_NE(join_inst, nullptr);
  EXPECT_TRUE(analysis.isUnambiguousJoin(join_inst));
  EXPECT_EQ(analysis.getPossibleJoinedForks(join_inst).size(), 1u);
}

TEST_F(JoinTargetAnalysisTest, ForeignJoinHandleDoesNotInventSingleTarget) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define i32 @main(i8* %foreign_tid) {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      %joined = call i32 @pthread_join(i8* %foreign_tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *join_inst = nullptr;
  for (const Instruction &inst : instructions(main_func)) {
    if (const auto *cb = dyn_cast<CallBase>(&inst)) {
      if (cb->getCalledFunction() &&
          cb->getCalledFunction()->getName() == "pthread_join") {
        join_inst = &inst;
        break;
      }
    }
  }

  ASSERT_NE(join_inst, nullptr);
  EXPECT_FALSE(analysis.isUnambiguousJoin(join_inst));
  EXPECT_TRUE(analysis.getPossibleJoinedForks(join_inst).empty());
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
