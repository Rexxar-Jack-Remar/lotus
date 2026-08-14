#include "AtomicTestSupport.h"

TEST_F(AtomicHappensBeforeTest, MatchingFencesEstablishHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 99, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      fence release
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

  const Instruction *store_data = &writer_func->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*reader_func, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_TRUE(hb.mustPrecede(store_data, load_data));
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
}
TEST_F(AtomicHappensBeforeTest,
       MatchingFencesWithAliasedAtomicPointersEstablishHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag_storage = global [1 x i32] zeroinitializer, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      %flag_ptr = getelementptr inbounds [1 x i32], [1 x i32]* @flag_storage, i64 0, i64 0
      store i32 7, i32* @data, align 4
      store atomic i32 1, i32* %flag_ptr release, align 4
      fence release
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %flag_alias = bitcast [1 x i32]* @flag_storage to i32*
      fence acquire
      %seen = load atomic i32, i32* %flag_alias acquire, align 4
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

  const Function *writer_func = module->getFunction("writer");
  const Function *reader_func = module->getFunction("reader");
  ASSERT_NE(writer_func, nullptr);
  ASSERT_NE(reader_func, nullptr);

  const Instruction *store_data = findStoreToGlobal(*writer_func, "data");
  const Instruction *load_data = findInstructionByName(*reader_func, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_TRUE(hb.mustPrecede(store_data, load_data));
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
}
TEST_F(AtomicHappensBeforeTest,
       DirectAliasedReleaseAcquireWithAliasAnalysisStaysDeferred) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag_storage = global [1 x i32] zeroinitializer, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      %flag_ptr = getelementptr inbounds [1 x i32], [1 x i32]* @flag_storage, i64 0, i64 0
      store i32 7, i32* @data, align 4
      store atomic i32 1, i32* %flag_ptr release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %flag_alias = bitcast [1 x i32]* @flag_storage to i32*
      %seen = load atomic i32, i32* %flag_alias acquire, align 4
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

  const Function *writer_func = module->getFunction("writer");
  const Function *reader_func = module->getFunction("reader");
  ASSERT_NE(writer_func, nullptr);
  ASSERT_NE(reader_func, nullptr);

  const Instruction *store_data = findStoreToGlobal(*writer_func, "data");
  const Instruction *load_data = findInstructionByName(*reader_func, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.setAliasAnalysis(mhp.getAliasAnalysis());
  hb.analyze();

  EXPECT_FALSE(hb.mustPrecede(store_data, load_data));
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
}
TEST_F(AtomicHappensBeforeTest, ReleaseSequenceThroughRmwSynchronizes) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 55, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @updater(i8* %arg) {
    entry:
      %old = atomicrmw add i32* @flag, i32 1 monotonic
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

  const Function *writer_func = module->getFunction("writer");
  const Function *reader_func = module->getFunction("reader");
  ASSERT_NE(writer_func, nullptr);
  ASSERT_NE(reader_func, nullptr);

  const Instruction *store_data = findStoreToGlobal(*writer_func, "data");
  const Instruction *load_data = findInstructionByName(*reader_func, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_FALSE(hb.mustPrecede(store_data, load_data));
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
}
TEST_F(AtomicHappensBeforeTest,
       ReleaseSequenceWithMultipleRmwTailsSynchronizes) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 66, i32* @data, align 4
      store atomic i32 1, i32* @flag release, align 4
      ret i8* null
    }

    define i8* @updater1(i8* %arg) {
    entry:
      %old1 = atomicrmw add i32* @flag, i32 1 monotonic
      ret i8* null
    }

    define i8* @updater2(i8* %arg) {
    entry:
      %old2 = atomicrmw add i32* @flag, i32 1 monotonic
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

  const Function *writer_func = module->getFunction("writer");
  const Function *reader_func = module->getFunction("reader");
  ASSERT_NE(writer_func, nullptr);
  ASSERT_NE(reader_func, nullptr);

  const Instruction *store_data = findStoreToGlobal(*writer_func, "data");
  const Instruction *load_data = findInstructionByName(*reader_func, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_FALSE(hb.mustPrecede(store_data, load_data));
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
}
TEST_F(AtomicHappensBeforeTest,
       MonotonicRmwWithoutReleaseHeadDoesNotSynchronize) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 91, i32* @data, align 4
      ret i8* null
    }

    define i8* @updater(i8* %arg) {
    entry:
      %old = atomicrmw add i32* @flag, i32 1 monotonic
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

  const Function *writer_func = module->getFunction("writer");
  const Function *reader_func = module->getFunction("reader");
  ASSERT_NE(writer_func, nullptr);
  ASSERT_NE(reader_func, nullptr);

  const Instruction *store_data = findStoreToGlobal(*writer_func, "data");
  const Instruction *load_data = findInstructionByName(*reader_func, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_FALSE(hb.mustPrecede(store_data, load_data));
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
}
TEST_F(AtomicHappensBeforeTest, AtomicRmwFenceWitnessEstablishesHB) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 77, i32* @data, align 4
      %old = atomicrmw xchg i32* @flag, i32 1 acq_rel
      fence release
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

  const Instruction *store_data = findStoreToGlobal(*writer_func, "data");
  const Instruction *load_data = findInstructionByName(*reader_func, "val");
  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  EXPECT_TRUE(hb.mustPrecede(store_data, load_data));
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
}
