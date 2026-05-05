#include "HappensBeforeAnalysisTestSupport.h"

TEST_F(HappensBeforeAnalysisTest,
       AssumeWitnessOnDifferentAtomicLocationDoesNotSynchronize) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag1 = global i32 0, align 4
    @flag2 = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @llvm.assume(i1)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 37, i32* @data, align 4
      store atomic i32 1, i32* @flag1 release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag2 acquire, align 4
      %ready = icmp ne i32 %seen, 0
      call void @llvm.assume(i1 %ready)
      %val = load i32, i32* @data, align 4
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}
TEST_F(HappensBeforeAnalysisTest,
       DirectReleaseAcquireWithBranchWitnessCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 7, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_data = &writer->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*reader, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
  EXPECT_FALSE(hb.happensBefore(load_data, store_data));
}
TEST_F(HappensBeforeAnalysisTest,
       BranchWitnessDoesNotOrderFalsePathContinuation) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 7, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %fallback

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    fallback:
      %fallback_val = load i32, i32* @data, align 4
      br label %exit

    exit:
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *true_load =
      findInstructionByName(*module->getFunction("reader"), "val");
  const Instruction *false_load =
      findInstructionByName(*module->getFunction("reader"), "fallback_val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(true_load, nullptr);
  ASSERT_NE(false_load, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, true_load));
  EXPECT_FALSE(hb.happensBefore(store_data, false_load));
}
TEST_F(HappensBeforeAnalysisTest,
       ReleaseStoreAcquireFenceWithBranchWitnessCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 13, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      fence acquire
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}
TEST_F(HappensBeforeAnalysisTest, BranchWitnessMustMatchConcreteReleaseValue) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp eq i32 %seen, 2
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}
TEST_F(HappensBeforeAnalysisTest,
       InitialAtomicValueMatchingWitnessDoesNotInventHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 1, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp eq i32 %seen, 1
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));

  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("atomic_witness_value_incompatible");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}
TEST_F(HappensBeforeAnalysisTest,
       ReleaseSequenceWithWitnessedConstantCanNowSynchronize) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 17, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @updater(i8* %arg) {
    entry:
      %old = atomicrmw add i32* @flag, i32 1 acq_rel
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @updater, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("atomic_release_sequence_edges_modeled");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}
TEST_F(HappensBeforeAnalysisTest,
       ReleaseSequenceWithMultipleTailsCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 27, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @updater1(i8* %arg) {
    entry:
      %old1 = atomicrmw add i32* @flag, i32 1 acq_rel
      ret i8* null
    }

    define i8* @updater2(i8* %arg) {
    entry:
      %old2 = atomicrmw add i32* @flag, i32 1 acq_rel
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      %tid4 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @updater1, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @updater2, i8* null)
      call i32 @pthread_create(i8* %tid4, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("atomic_release_sequence_edges_modeled");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}
TEST_F(HappensBeforeAnalysisTest,
       MixedFenceReleaseSequenceWithoutReadsFromProofIsDeferred) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 31, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @updater(i8* %arg) {
    entry:
      %old = atomicrmw add i32* @flag, i32 1 acq_rel
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      fence acquire
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @updater, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
  const auto &deferred = hb.getDeferredSyncCounts();
  size_t modeled_edges = 0;
  auto mixed_fence_it = deferred.find("atomic_mixed_fence_edges_modeled");
  if (mixed_fence_it != deferred.end()) {
    modeled_edges += mixed_fence_it->second;
  }
  auto release_sequence_it =
      deferred.find("atomic_release_sequence_edges_modeled");
  if (release_sequence_it != deferred.end()) {
    modeled_edges += release_sequence_it->second;
  }
  EXPECT_GT(modeled_edges, 0u);
}
TEST_F(HappensBeforeAnalysisTest,
       LaterNonReleaseStoreDoesNotInheritEarlierReleaseHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 19, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @overwriter(i8* %arg) {
    entry:
      store atomic i32 2, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @overwriter, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *store_data =
      &module->getFunction("writer")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}
TEST_F(HappensBeforeAnalysisTest, ReleaseFenceStoreAcquireFenceCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      fence release
      store atomic i32 1, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      fence acquire
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_data = &writer->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*reader, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
  EXPECT_FALSE(hb.happensBefore(load_data, store_data));
}
TEST_F(HappensBeforeAnalysisTest,
       ReleaseFenceStoreAcquireFenceDoesNotOrderWitnessLoad) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      fence release
      store atomic i32 1, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      fence acquire
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_data = &writer->getEntryBlock().front();
  const Instruction *seen = findInstructionByName(*reader, "seen");
  const Instruction *load_data = findInstructionByName(*reader, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(seen, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, seen));
  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}
TEST_F(HappensBeforeAnalysisTest,
       ReleaseFenceStoreAcquireFenceAcrossBlocksCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      fence release
      store atomic i32 1, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      br label %fence_bb

    fence_bb:
      fence acquire
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_data = &writer->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*reader, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}
TEST_F(HappensBeforeAnalysisTest,
       ReleaseFenceAcrossBlocksBeforeStoreCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      br label %publish

    publish:
      fence release
      br label %store_bb

    store_bb:
      store atomic i32 1, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      fence acquire
      %ready = icmp ne i32 %seen, 0
      br i1 %ready, label %read, label %exit

    read:
      %val = load i32, i32* @data, align 4
      br label %exit

    exit:
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *writer = module->getFunction("writer");
  const Function *reader = module->getFunction("reader");
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);

  const Instruction *store_data = &writer->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*reader, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_data, load_data));
}
