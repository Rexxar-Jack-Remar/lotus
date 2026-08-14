TEST_F(HappensBeforeAnalysisTest,
       LoopReleaseSequenceTailsStayDeferred) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @publisher(i8* %arg) {
    entry:
      store i32 77, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      br label %loop

    loop:
      %old1 = atomicrmw add i32* @flag, i32 1 monotonic
      %old2 = atomicrmw add i32* @flag, i32 1 monotonic
      %again = icmp eq i32 %old2, -1
      br i1 %again, label %loop, label %exit

    exit:
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp eq i32 %seen, 3
      br i1 %ready, label %read, label %exit

    read:
      %value = load i32, i32* @data, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %publisher_tid = alloca i8
      %reader_tid = alloca i8
      call i32 @pthread_create(i8* %publisher_tid, i8* null,
                               i8* (i8*)* @publisher, i8* null)
      call i32 @pthread_create(i8* %reader_tid, i8* null,
                               i8* (i8*)* @reader, i8* null)
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
      &module->getFunction("publisher")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "value");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);
  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
  const auto &deferred = hb.getDeferredSyncCounts();
  auto it = deferred.find("atomic_release_sequence_tail_order_ambiguous");
  ASSERT_NE(it, deferred.end());
  EXPECT_GE(it->second, 1u);
}

TEST_F(HappensBeforeAnalysisTest,
       MultiStepReleaseSequenceCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @publisher(i8* %arg) {
    entry:
      store i32 77, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @rmw1(i8* %arg) {
    entry:
      %old1 = atomicrmw add i32* @flag, i32 1 monotonic
      ret i8* null
    }

    define i8* @rmw2(i8* %arg) {
    entry:
      %old2 = atomicrmw add i32* @flag, i32 1 monotonic
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ready = icmp eq i32 %seen, 3
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
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @publisher, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @rmw1, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @rmw2, i8* null)
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
      &module->getFunction("publisher")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}
TEST_F(HappensBeforeAnalysisTest, UnknownAtomicInitialValueCannotProveReadsFrom) {
  const char *source = R"(
    @flag = external global i32
    @shared = global i32 0
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    define i8* @writer(i8* %arg) {
      store i32 1, i32* @shared
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }
    define i8* @reader(i8* %arg) {
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ok = icmp eq i32 %seen, 1
      br i1 %ok, label %yes, label %no
    yes:
      %value = load i32, i32* @shared
      ret i8* null
    no:
      ret i8* null
    }
    define i32 @main() {
      %a = alloca i8
      %b = alloca i8
      call i32 @pthread_create(i8* %a, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %b, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();
  EXPECT_FALSE(hb.happensBefore(
      &module->getFunction("writer")->front().front(),
      findInstructionByName(*module->getFunction("reader"), "value")));
}

TEST_F(HappensBeforeAnalysisTest,
       AcquireFenceRequiresAnchorLoadValueToMatchRelease) {
  const char *source = R"(
    @flag = global i32 0
    @shared = global i32 0
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    define i8* @writer(i8* %arg) {
      store i32 1, i32* @shared
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }
    define i8* @reader(i8* %arg) {
      %seen = load atomic i32, i32* @flag monotonic, align 4
      fence acquire
      %old = icmp eq i32 %seen, 0
      br i1 %old, label %yes, label %no
    yes:
      %value = load i32, i32* @shared
      ret i8* null
    no:
      ret i8* null
    }
    define i32 @main() {
      %a = alloca i8
      %b = alloca i8
      call i32 @pthread_create(i8* %a, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %b, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();
  EXPECT_FALSE(hb.happensBefore(
      &module->getFunction("writer")->front().front(),
      findInstructionByName(*module->getFunction("reader"), "value")));
}

TEST_F(HappensBeforeAnalysisTest,
       CrossThreadRmwIsNotAssumedToFollowReleaseHead) {
  const char *source = R"(
    @flag = global i32 0
    @shared = global i32 0
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    define i8* @writer(i8* %arg) {
      store i32 1, i32* @shared
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }
    define i8* @tail(i8* %arg) {
      atomicrmw xchg i32* @flag, i32 2 monotonic
      ret i8* null
    }
    define i8* @reader(i8* %arg) {
      %seen = load atomic i32, i32* @flag acquire, align 4
      %ok = icmp eq i32 %seen, 2
      br i1 %ok, label %yes, label %no
    yes:
      %value = load i32, i32* @shared
      ret i8* null
    no:
      ret i8* null
    }
    define i32 @main() {
      %a = alloca i8
      %b = alloca i8
      %c = alloca i8
      call i32 @pthread_create(i8* %a, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %b, i8* null, i8* (i8*)* @tail, i8* null)
      call i32 @pthread_create(i8* %c, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();
  EXPECT_FALSE(hb.happensBefore(
      &module->getFunction("writer")->front().front(),
      findInstructionByName(*module->getFunction("reader"), "value")));
}
TEST_F(HappensBeforeAnalysisTest,
       NonAdjacentFenceAtomicPatternInSameBlockCreatesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 9, i32* @data, align 4
      fence release
      %tmp = add i32 1, 2
      store atomic i32 %tmp, i32* @flag monotonic, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
      %tmp = add i32 %seen, 1
      fence acquire
      %ready = icmp ne i32 %tmp, 0
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
TEST_F(HappensBeforeAnalysisTest, CompetingReleaseStoresDoNotInventAtomicHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer1(i8* %arg) {
    entry:
      store i32 1, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @writer2(i8* %arg) {
    entry:
      store atomic i32 2, i32* @flag release, align 4
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
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @writer1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @writer2, i8* null)
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
      &module->getFunction("writer1")->getEntryBlock().front();
  const Instruction *load_data =
      findInstructionByName(*module->getFunction("reader"), "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}
TEST_F(HappensBeforeAnalysisTest, CrossLocationAtomicsDoNotSynchronize) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag1 = global i32 0, align 4
    @flag2 = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 5, i32* @data, align 4
      store atomic i32 1, i32* @flag1 release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag2 acquire, align 4
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

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}
TEST_F(HappensBeforeAnalysisTest, RelaxedLoadWithoutFenceDoesNotSynchronize) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 11, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %seen = load atomic i32, i32* @flag monotonic, align 4
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

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}
TEST_F(HappensBeforeAnalysisTest,
       SameAggregateDifferentAtomicFieldsDoNotSynchronize) {
  const char *source = R"(
    %Pair = type { i32, i32 }

    @data = global i32 0, align 4
    @pair = global %Pair zeroinitializer, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      %first = getelementptr inbounds %Pair, %Pair* @pair, i32 0, i32 0
      store i32 9, i32* @data, align 4
      store atomic i32 1, i32* %first release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %second = getelementptr inbounds %Pair, %Pair* @pair, i32 0, i32 1
      %seen = load atomic i32, i32* %second acquire, align 4
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

  EXPECT_FALSE(hb.happensBefore(store_data, load_data));
}
