#include "Analysis/Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Analysis/Concurrency/MHP/MHPAnalysis.h"
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;

class AtomicHappensBeforeTest : public ::testing::Test {
protected:
  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("AtomicHappensBeforeTest", errs());
    }
    return module;
  }

  const Instruction *findInstructionByName(const Function &func, StringRef name) {
      for (const auto &bb : func) {
          for (const auto &inst : bb) {
              if (inst.getName() == name) {
                  return &inst;
              }
          }
      }
      return nullptr;
  }

  const Instruction *findStoreToGlobal(const Function &func, StringRef global_name) {
    for (const auto &bb : func) {
      for (const auto &inst : bb) {
        const auto *store = dyn_cast<StoreInst>(&inst);
        if (!store) {
          continue;
        }
        const Value *ptr = store->getPointerOperand()->stripPointerCasts();
        if (const auto *gv = dyn_cast<GlobalVariable>(ptr)) {
          if (gv->getName() == global_name) {
            return &inst;
          }
        }
      }
    }
    return nullptr;
  }
};

TEST_F(AtomicHappensBeforeTest, ReleaseAcquireOrdering) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i8 0, align 1

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 42, i32* @data, align 4
      store atomic i8 1, i8* @flag release, align 1
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %load_flag = load atomic i8, i8* @flag acquire, align 1
      %cond = icmp ne i8 %load_flag, 0
      br i1 %cond, label %if.then, label %if.end

    if.then:
      %load_data = load i32, i32* @data, align 4
      br label %if.end

    if.end:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %writer_tid = alloca i8
      %reader_tid = alloca i8
      call i32 @pthread_create(i8* %writer_tid, i8* null, i8* (i8*)* @writer, i8* null)
      call i32 @pthread_create(i8* %reader_tid, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  
  const Function *writer_func = module->getFunction("writer");
  const Function *reader_func = module->getFunction("reader");
  ASSERT_NE(writer_func, nullptr);
  ASSERT_NE(reader_func, nullptr);

  // Get instructions by position/name
  const Instruction* store_data = &writer_func->getEntryBlock().front();
  const Instruction* load_data = findInstructionByName(*reader_func, "load_data");

  ASSERT_NE(store_data, nullptr);
  ASSERT_TRUE(isa<StoreInst>(store_data));
  ASSERT_NE(load_data, nullptr);
  
  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  // Without a reads-from witness, release/acquire on @flag is not enough for
  // the analysis to prove a definite HB edge for the non-atomic accesses.
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
  EXPECT_FALSE(hb.mustPrecede(store_data, load_data));
}

TEST_F(AtomicHappensBeforeTest, SequentialConsistency) {
  const char *source = R"(
    @data = global i32 0, align 4
    @sync = global i8 0, align 1

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @thread1(i8* %arg) {
    entry:
      store i32 100, i32* @data, align 4
      store atomic i8 1, i8* @sync seq_cst, align 1
      ret i8* null
    }

    define i8* @thread2(i8* %arg) {
    entry:
      %flag = load atomic i8, i8* @sync seq_cst, align 1
      %cond = icmp ne i8 %flag, 0
      br i1 %cond, label %if.then, label %if.end

    if.then:
      %val = load i32, i32* @data, align 4
      br label %if.end

    if.end:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @thread1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @thread2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *thread1_func = module->getFunction("thread1");
  const Function *thread2_func = module->getFunction("thread2");
  ASSERT_NE(thread1_func, nullptr);
  ASSERT_NE(thread2_func, nullptr);

  const Instruction *store_data = &thread1_func->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*thread2_func, "val");

  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  // Seq-cst alone does not let the analysis prove a cross-thread HB edge
  // without a concrete reads-from witness.
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
  EXPECT_FALSE(hb.mustPrecede(store_data, load_data));
}

TEST_F(AtomicHappensBeforeTest, RelaxedAtomicsNoSynchronization) {
  const char *source = R"(
    @data = global i32 0, align 4
    @counter = global i8 0, align 1

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 42, i32* @data, align 4
      store atomic i8 1, i8* @counter monotonic, align 1
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %cnt = load atomic i8, i8* @counter monotonic, align 1
      %cond = icmp ne i8 %cnt, 0
      br i1 %cond, label %if.then, label %if.end

    if.then:
      %val = load i32, i32* @data, align 4
      br label %if.end

    if.end:
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

  // Relaxed/monotonic atomics don't provide synchronization
  // The store and load may happen in parallel (data race)
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
}

TEST_F(AtomicHappensBeforeTest, AcquireReleaseOrdering) {
  const char *source = R"(
    @data1 = global i32 0, align 4
    @data2 = global i32 0, align 4
    @sync = global i8 0, align 1

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @producer(i8* %arg) {
    entry:
      store i32 10, i32* @data1, align 4
      store i32 20, i32* @data2, align 4
      store atomic i8 1, i8* @sync release, align 1
      ret i8* null
    }

    define i8* @consumer(i8* %arg) {
    entry:
      %flag = load atomic i8, i8* @sync acquire, align 1
      %cond = icmp ne i8 %flag, 0
      br i1 %cond, label %if.then, label %if.end

    if.then:
      %v1 = load i32, i32* @data1, align 4
      %v2 = load i32, i32* @data2, align 4
      br label %if.end

    if.end:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @producer, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @consumer, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *producer_func = module->getFunction("producer");
  const Function *consumer_func = module->getFunction("consumer");
  ASSERT_NE(producer_func, nullptr);
  ASSERT_NE(consumer_func, nullptr);

  // Find stores by iterating
  const Instruction *store_data1 = nullptr;
  const Instruction *store_data2 = nullptr;
  for (const auto &bb : *producer_func) {
    for (const auto &inst : bb) {
      if (isa<StoreInst>(&inst) && !inst.isAtomic()) {
        if (!store_data1) {
          store_data1 = &inst;
        } else if (!store_data2) {
          store_data2 = &inst;
        }
      }
    }
  }

  const Instruction *load_data1 = findInstructionByName(*consumer_func, "v1");
  const Instruction *load_data2 = findInstructionByName(*consumer_func, "v2");

  ASSERT_NE(store_data1, nullptr);
  ASSERT_NE(store_data2, nullptr);
  ASSERT_NE(load_data1, nullptr);
  ASSERT_NE(load_data2, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  // These atomic pairs are synchronization candidates, but the analysis no
  // longer turns them into definite HB edges without reads-from evidence.
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data1, load_data1));
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data2, load_data2));
  EXPECT_FALSE(hb.mustPrecede(store_data1, load_data1));
  EXPECT_FALSE(hb.mustPrecede(store_data2, load_data2));
}

TEST_F(AtomicHappensBeforeTest, MultipleAtomicVariables) {
  const char *source = R"(
    @x = global i32 0, align 4
    @y = global i32 0, align 4
    @flag1 = global i8 0, align 1
    @flag2 = global i8 0, align 1

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @thread1(i8* %arg) {
    entry:
      store i32 1, i32* @x, align 4
      store atomic i8 1, i8* @flag1 release, align 1
      store i32 2, i32* @y, align 4
      store atomic i8 1, i8* @flag2 release, align 1
      ret i8* null
    }

    define i8* @thread2(i8* %arg) {
    entry:
      %f1 = load atomic i8, i8* @flag1 acquire, align 1
      %c1 = icmp ne i8 %f1, 0
      br i1 %c1, label %read_x, label %end

    read_x:
      %vx = load i32, i32* @x, align 4
      %f2 = load atomic i8, i8* @flag2 acquire, align 1
      %c2 = icmp ne i8 %f2, 0
      br i1 %c2, label %read_y, label %end

    read_y:
      %vy = load i32, i32* @y, align 4
      br label %end

    end:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @thread1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @thread2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *thread1_func = module->getFunction("thread1");
  const Function *thread2_func = module->getFunction("thread2");
  ASSERT_NE(thread1_func, nullptr);
  ASSERT_NE(thread2_func, nullptr);

  // Find stores and loads
  const Instruction *store_x = nullptr;
  const Instruction *store_y = nullptr;
  for (const auto &bb : *thread1_func) {
    for (const auto &inst : bb) {
      if (isa<StoreInst>(&inst) && !inst.isAtomic()) {
        if (inst.getOperand(1) == module->getGlobalVariable("x")) {
          store_x = &inst;
        } else if (inst.getOperand(1) == module->getGlobalVariable("y")) {
          store_y = &inst;
        }
      }
    }
  }

  const Instruction *load_x = findInstructionByName(*thread2_func, "vx");
  const Instruction *load_y = findInstructionByName(*thread2_func, "vy");

  ASSERT_NE(store_x, nullptr);
  ASSERT_NE(store_y, nullptr);
  ASSERT_NE(load_x, nullptr);
  ASSERT_NE(load_y, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  // Same-location release/acquire operations are not upgraded to definite HB
  // edges without a reads-from witness, so both non-atomic pairs remain MHP.
  EXPECT_TRUE(mhp.mayHappenInParallel(store_x, load_x));
  EXPECT_TRUE(mhp.mayHappenInParallel(store_y, load_y));
  EXPECT_FALSE(hb.mustPrecede(store_x, load_x));
  EXPECT_FALSE(hb.mustPrecede(store_y, load_y));
}

TEST_F(AtomicHappensBeforeTest, AtomicChain) {
  const char *source = R"(
    @data = global i32 0, align 4
    @sync1 = global i8 0, align 1
    @sync2 = global i8 0, align 1

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @thread1(i8* %arg) {
    entry:
      store i32 100, i32* @data, align 4
      store atomic i8 1, i8* @sync1 release, align 1
      ret i8* null
    }

    define i8* @thread2(i8* %arg) {
    entry:
      %f1 = load atomic i8, i8* @sync1 acquire, align 1
      %c1 = icmp ne i8 %f1, 0
      br i1 %c1, label %forward, label %end

    forward:
      store atomic i8 1, i8* @sync2 release, align 1
      br label %end

    end:
      ret i8* null
    }

    define i8* @thread3(i8* %arg) {
    entry:
      %f2 = load atomic i8, i8* @sync2 acquire, align 1
      %c2 = icmp ne i8 %f2, 0
      br i1 %c2, label %read, label %end

    read:
      %val = load i32, i32* @data, align 4
      br label %end

    end:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @thread1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @thread2, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @thread3, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *thread1_func = module->getFunction("thread1");
  const Function *thread3_func = module->getFunction("thread3");
  ASSERT_NE(thread1_func, nullptr);
  ASSERT_NE(thread3_func, nullptr);

  const Instruction *store_data = &thread1_func->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*thread3_func, "val");

  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  // The analysis does not build transitive HB through atomic chains without
  // concrete reads-from evidence at each synchronization step.
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
  EXPECT_FALSE(hb.mustPrecede(store_data, load_data));
}

TEST_F(AtomicHappensBeforeTest, CompareAndSwap) {
  const char *source = R"(
    @data = global i32 0, align 4
    @atomic_var = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @updater(i8* %arg) {
    entry:
      store i32 42, i32* @data, align 4
      %old = cmpxchg i32* @atomic_var, i32 0, i32 1 acq_rel monotonic
      store atomic i32 1, i32* @atomic_var release, align 4
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %val = load atomic i32, i32* @atomic_var acquire, align 4
      %cond = icmp eq i32 %val, 1
      br i1 %cond, label %if.then, label %if.end

    if.then:
      %data_val = load i32, i32* @data, align 4
      br label %if.end

    if.end:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @updater, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @reader, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *updater_func = module->getFunction("updater");
  const Function *reader_func = module->getFunction("reader");
  ASSERT_NE(updater_func, nullptr);
  ASSERT_NE(reader_func, nullptr);

  const Instruction *store_data = &updater_func->getEntryBlock().front();
  const Instruction *load_data = findInstructionByName(*reader_func, "data_val");

  ASSERT_NE(store_data, nullptr);
  ASSERT_NE(load_data, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  // Even with CAS/RMW, the analysis now requires a definite reads-from witness
  // before proving a cross-thread HB edge.
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
  EXPECT_FALSE(hb.mustPrecede(store_data, load_data));
}

TEST_F(AtomicHappensBeforeTest, NoSynchronizationWithoutMatchingOrdering) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag = global i8 0, align 1

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 42, i32* @data, align 4
      store atomic i8 1, i8* @flag monotonic, align 1
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      %flag_val = load atomic i8, i8* @flag monotonic, align 1
      %cond = icmp ne i8 %flag_val, 0
      br i1 %cond, label %if.then, label %if.end

    if.then:
      %val = load i32, i32* @data, align 4
      br label %if.end

    if.end:
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

  // Monotonic ordering doesn't provide synchronization
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
}

TEST_F(AtomicHappensBeforeTest, SequentialConsistencyMultipleThreads) {
  const char *source = R"(
    @x = global i32 0, align 4
    @y = global i32 0, align 4
    @sync = global i8 0, align 1

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @thread1(i8* %arg) {
    entry:
      store i32 1, i32* @x, align 4
      store atomic i8 1, i8* @sync seq_cst, align 1
      ret i8* null
    }

    define i8* @thread2(i8* %arg) {
    entry:
      store i32 2, i32* @y, align 4
      store atomic i8 1, i8* @sync seq_cst, align 1
      ret i8* null
    }

    define i8* @thread3(i8* %arg) {
    entry:
      %flag1 = load atomic i8, i8* @sync seq_cst, align 1
      %flag2 = load atomic i8, i8* @sync seq_cst, align 1
      %both_set = and i8 %flag1, %flag2
      %cond = icmp ne i8 %both_set, 0
      br i1 %cond, label %read, label %end

    read:
      %vx = load i32, i32* @x, align 4
      %vy = load i32, i32* @y, align 4
      br label %end

    end:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      %tid3 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @thread1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @thread2, i8* null)
      call i32 @pthread_create(i8* %tid3, i8* null, i8* (i8*)* @thread3, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *thread1_func = module->getFunction("thread1");
  const Function *thread2_func = module->getFunction("thread2");
  const Function *thread3_func = module->getFunction("thread3");
  ASSERT_NE(thread1_func, nullptr);
  ASSERT_NE(thread2_func, nullptr);
  ASSERT_NE(thread3_func, nullptr);

  const Instruction *store_x = &thread1_func->getEntryBlock().front();
  const Instruction *store_y = &thread2_func->getEntryBlock().front();
  const Instruction *load_x = findInstructionByName(*thread3_func, "vx");
  const Instruction *load_y = findInstructionByName(*thread3_func, "vy");

  ASSERT_NE(store_x, nullptr);
  ASSERT_NE(store_y, nullptr);
  ASSERT_NE(load_x, nullptr);
  ASSERT_NE(load_y, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  // Seq-cst does not by itself establish definite HB from these stores to the
  // later loads without a reads-from witness.
  EXPECT_TRUE(mhp.mayHappenInParallel(store_x, load_x));
  EXPECT_TRUE(mhp.mayHappenInParallel(store_y, load_y));
}

TEST_F(AtomicHappensBeforeTest, UnrelatedFencesDoNotSynchronize) {
  const char *source = R"(
    @data = global i32 0, align 4
    @flag1 = global i32 0, align 4
    @flag2 = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @writer(i8* %arg) {
    entry:
      store i32 7, i32* @data, align 4
      store atomic i32 1, i32* @flag1 release, align 4
      fence release
      ret i8* null
    }

    define i8* @reader(i8* %arg) {
    entry:
      fence acquire
      %flag = load atomic i32, i32* @flag2 acquire, align 4
      %cond = icmp ne i32 %flag, 0
      br i1 %cond, label %read, label %exit

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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
}

TEST_F(AtomicHappensBeforeTest, SeqCstDifferentLocationsStayParallel) {
  const char *source = R"(
    @x = global i32 0, align 4
    @y = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @thread1(i8* %arg) {
    entry:
      store i32 1, i32* @x, align 4
      store atomic i32 1, i32* @x seq_cst, align 4
      ret i8* null
    }

    define i8* @thread2(i8* %arg) {
    entry:
      store i32 2, i32* @y, align 4
      %flag = load atomic i32, i32* @y seq_cst, align 4
      %cond = icmp ne i32 %flag, 0
      br i1 %cond, label %read, label %exit

    read:
      %val = load i32, i32* @x, align 4
      br label %exit

    exit:
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @thread1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @thread2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *thread1_func = module->getFunction("thread1");
  const Function *thread2_func = module->getFunction("thread2");
  ASSERT_NE(thread1_func, nullptr);
  ASSERT_NE(thread2_func, nullptr);

  const Instruction *store_x = &thread1_func->getEntryBlock().front();
  const Instruction *load_x = findInstructionByName(*thread2_func, "val");

  MHPAnalysis mhp(*module);
  mhp.analyze();

  EXPECT_TRUE(mhp.mayHappenInParallel(store_x, load_x));
}

TEST_F(AtomicHappensBeforeTest, MatchingFencesCreateHappensBefore) {
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

TEST_F(AtomicHappensBeforeTest, MatchingFencesWithAliasedAtomicPointersCreateHB) {
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
  hb.analyze();

  EXPECT_TRUE(hb.mustPrecede(store_data, load_data));
  EXPECT_TRUE(mhp.mayHappenInParallel(store_data, load_data));
}

// Main function for tests
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
