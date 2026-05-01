#include "JoinTargetAnalysisTestSupport.h"

TEST_F(JoinTargetAnalysisTest, WrapperStoreIntoPointerSlotResolvesCallerJoin) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @spawn_into_slot(i8** %slot, i8* %tid) {
    entry:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      store i8* %tid, i8** %slot
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i8*
      %tid = alloca i8
      call void @spawn_into_slot(i8** %slot, i8* %tid)
      %join_tid = load i8*, i8** %slot
      call i32 @pthread_join(i8* %join_tid, i8* null)
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
  EXPECT_EQ(analysis.getFeasibleJoinedForks(join_inst).size(), 1u);
  EXPECT_NE(analysis.getDefiniteFeasibleJoinedFork(join_inst), nullptr);
}
TEST_F(JoinTargetAnalysisTest, WrapperJoinThroughPointerSlotClearsCallerState) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @join_from_slot(i8** %slot) {
    entry:
      %join_tid = load i8*, i8** %slot
      call i32 @pthread_join(i8* %join_tid, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      %slot = alloca i8*
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      store i8* %tid, i8** %slot
      call void @join_from_slot(i8** %slot)
      %join_tid2 = load i8*, i8** %slot
      call i32 @pthread_join(i8* %join_tid2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  JoinTargetAnalysis analysis(*module);
  analysis.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  SmallVector<const Instruction *, 2> joins;
  for (const Instruction &inst : instructions(main_func)) {
    if (const auto *cb = dyn_cast<CallBase>(&inst)) {
      if (cb->getCalledFunction() &&
          cb->getCalledFunction()->getName() == "pthread_join") {
        joins.push_back(&inst);
      }
    }
  }

  ASSERT_EQ(joins.size(), 1u);
  EXPECT_TRUE(analysis.getFeasibleJoinedForks(joins[0]).empty());
  EXPECT_FALSE(analysis.isUnambiguousJoin(joins[0]));
}
TEST_F(JoinTargetAnalysisTest, WrapperFieldStoreKeepsDistinctFieldsSeparated) {
  const char *source = R"(
    %pair = type { i8*, i8* }

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

    define void @store_into_first(%pair* %slots, i8* %tid, i8* (i8*)* %fn) {
    entry:
      %field0 = getelementptr inbounds %pair, %pair* %slots, i32 0, i32 0
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* %fn, i8* null)
      store i8* %tid, i8** %field0
      ret void
    }

    define void @store_into_second(%pair* %slots, i8* %tid, i8* (i8*)* %fn) {
    entry:
      %field1 = getelementptr inbounds %pair, %pair* %slots, i32 0, i32 1
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* %fn, i8* null)
      store i8* %tid, i8** %field1
      ret void
    }

    define i32 @main() {
    entry:
      %slots = alloca %pair
      %tid1 = alloca i8
      %tid2 = alloca i8
      call void @store_into_first(%pair* %slots, i8* %tid1, i8* (i8*)* @worker1)
      call void @store_into_second(%pair* %slots, i8* %tid2, i8* (i8*)* @worker2)
      %field0 = getelementptr inbounds %pair, %pair* %slots, i32 0, i32 0
      %join_tid = load i8*, i8** %field0
      call i32 @pthread_join(i8* %join_tid, i8* null)
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
  EXPECT_EQ(analysis.getFeasibleJoinedForks(join_inst).size(), 1u);
  EXPECT_NE(analysis.getDefiniteFeasibleJoinedFork(join_inst), nullptr);
}
TEST_F(JoinTargetAnalysisTest,
       JoinResolutionMarksRepeatedForkSiteAsAmbiguous) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define void @spawn_helper(i8* %tid) {
    entry:
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      ret void
    }

    define i32 @main(i1 %cond) {
    entry:
      %tid = alloca i8
      br i1 %cond, label %left, label %right

    left:
      call void @spawn_helper(i8* %tid)
      br label %join

    right:
      call void @spawn_helper(i8* %tid)
      br label %join

    join:
      call i32 @pthread_join(i8* %tid, i8* null)
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
  JoinResolution resolution = analysis.getJoinResolution(join_inst);
  EXPECT_FALSE(resolution.unambiguous);
  ASSERT_EQ(resolution.feasible_instances.size(), 1u);
  EXPECT_EQ(resolution.feasible_instances.front().execution_class,
            ThreadExecutionClass::RepeatedExecution);
  EXPECT_NE(std::find(resolution.ambiguity_reasons.begin(),
                      resolution.ambiguity_reasons.end(),
                      JoinAmbiguityReason::RepeatedForkSite),
            resolution.ambiguity_reasons.end());
}
TEST_F(JoinTargetAnalysisTest, JoinResolutionCarriesPathAlternativesForSelect) {
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
      call i32 @pthread_join(i8* %join_tid, i8* null)
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
  JoinResolution resolution = analysis.getJoinResolution(join_inst);
  EXPECT_TRUE(resolution.is_path_sensitive);
  EXPECT_EQ(resolution.path_alternatives.size(), 2u);
  EXPECT_EQ(resolution.feasible_instances.size(), 2u);
  EXPECT_NE(std::find(resolution.ambiguity_reasons.begin(),
                      resolution.ambiguity_reasons.end(),
                      JoinAmbiguityReason::PathMergedAlternatives),
            resolution.ambiguity_reasons.end());
}
TEST_F(JoinTargetAnalysisTest,
       JoinResolutionReportsNoFeasibleInstanceAfterDetach) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_detach(i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call i32 @pthread_detach(i8* %tid)
      call i32 @pthread_join(i8* %tid, i8* null)
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
  JoinResolution resolution = analysis.getJoinResolution(join_inst);
  EXPECT_TRUE(resolution.feasible_instances.empty());
  EXPECT_NE(std::find(resolution.ambiguity_reasons.begin(),
                      resolution.ambiguity_reasons.end(),
                      JoinAmbiguityReason::NoFeasibleInstance),
            resolution.ambiguity_reasons.end());
}
