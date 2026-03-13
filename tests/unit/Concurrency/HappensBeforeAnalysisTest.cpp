#include "Analysis/Concurrency/MHP/HappensBeforeAnalysis.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace mhp;
using namespace lotus;

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

class HappensBeforeAnalysisTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("HappensBeforeAnalysisTest", errs());
    }
    return module;
  }
};

TEST_F(HappensBeforeAnalysisTest, CallOnceDoesNotCreateBidirectionalHB) {
  const char *source = R"(
    @flag = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare void @std_call_once(i8*)

    define i8* @worker1(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag)
      %w1 = add i32 1, 2
      ret i8* null
    }

    define i8* @worker2(i8* %arg) {
    entry:
      call void @std_call_once(i8* @flag)
      %w2 = add i32 3, 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid1 = alloca i8
      %tid2 = alloca i8
      call i32 @pthread_create(i8* %tid1, i8* null, i8* (i8*)* @worker1, i8* null)
      call i32 @pthread_create(i8* %tid2, i8* null, i8* (i8*)* @worker2, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *worker1 = module->getFunction("worker1");
  const Function *worker2 = module->getFunction("worker2");
  ASSERT_NE(worker1, nullptr);
  ASSERT_NE(worker2, nullptr);

  const Instruction *w1 = findInstructionByName(*worker1, "w1");
  const Instruction *w2 = findInstructionByName(*worker2, "w2");
  ASSERT_NE(w1, nullptr);
  ASSERT_NE(w2, nullptr);

  EXPECT_FALSE(hb.happensBefore(w1, w2) && hb.happensBefore(w2, w1));
}

TEST_F(HappensBeforeAnalysisTest, PromiseFutureTracksSharedState) {
  const char *source = R"(
    @shared = global i32 0, align 4
    @promise_obj = global i8 0

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i8* @_ZNSt7promise10get_futureEv(i8*)
    declare void @_ZNSt7promise9set_valueEv(i8*)
    declare void @_ZNSt6future3getEv(i8*)

    define i8* @producer(i8* %unused) {
    entry:
      store i32 99, i32* @shared, align 4
      call void @_ZNSt7promise9set_valueEv(i8* @promise_obj)
      ret i8* null
    }

    define i8* @consumer(i8* %unused) {
    entry:
      %future = call i8* @_ZNSt7promise10get_futureEv(i8* @promise_obj)
      call void @_ZNSt6future3getEv(i8* %future)
      %val = load i32, i32* @shared, align 4
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

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer");
  const Function *consumer = module->getFunction("consumer");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared = findInstructionByName(*consumer, "val");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, OpenMPTaskDependenciesContributeToHB) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps_out = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]
    @deps_in = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 1
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %dep_out = getelementptr inbounds [1 x %kmp_depend_info],
                 [1 x %kmp_depend_info]* @deps_out, i64 0, i64 0
      %dep_in = getelementptr inbounds [1 x %kmp_depend_info],
                [1 x %kmp_depend_info]* @deps_in, i64 0, i64 0
      %t1 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep_out, i32 0, %kmp_depend_info* null)
      %t2 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep_in, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  const Instruction *t1 = findInstructionByName(*main_func, "t1");
  const Instruction *t2 = findInstructionByName(*main_func, "t2");
  ASSERT_NE(t1, nullptr);
  ASSERT_NE(t2, nullptr);

  EXPECT_TRUE(hb.happensBefore(t1, t2));
}

TEST_F(HappensBeforeAnalysisTest, OpenMPTaskBodyDependenciesContributeToHB) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps_out = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]
    @deps_in = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 1
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i8* @producer_task(i8* %arg) {
    entry:
      store i32 7, i32* @shared, align 4
      ret i8* null
    }

    define i8* @consumer_task(i8* %arg) {
    entry:
      %loaded = load i32, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %task1 = alloca i8* (i8*)*, align 8
      %task2 = alloca i8* (i8*)*, align 8
      store i8* (i8*)* @producer_task, i8* (i8*)** %task1, align 8
      store i8* (i8*)* @consumer_task, i8* (i8*)** %task2, align 8
      %task1_raw = bitcast i8* (i8*)** %task1 to i8*
      %task2_raw = bitcast i8* (i8*)** %task2 to i8*
      %dep_out = getelementptr inbounds [1 x %kmp_depend_info],
                 [1 x %kmp_depend_info]* @deps_out, i64 0, i64 0
      %dep_in = getelementptr inbounds [1 x %kmp_depend_info],
                [1 x %kmp_depend_info]* @deps_in, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* %task1_raw, i32 1,
          %kmp_depend_info* %dep_out, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* %task2_raw, i32 1,
          %kmp_depend_info* %dep_in, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *producer = module->getFunction("producer_task");
  const Function *consumer = module->getFunction("consumer_task");
  ASSERT_NE(producer, nullptr);
  ASSERT_NE(consumer, nullptr);

  const Instruction *store_shared = &producer->getEntryBlock().front();
  const Instruction *load_shared =
      findInstructionByName(*consumer, "loaded");
  ASSERT_NE(store_shared, nullptr);
  ASSERT_NE(load_shared, nullptr);

  EXPECT_TRUE(hb.happensBefore(store_shared, load_shared));
}

TEST_F(HappensBeforeAnalysisTest, OpenMPSingleBoundaryOrdersTaskContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_single(i8*, i32)
    declare void @__kmpc_end_single(i8*, i32)

    define i8* @task_body(i8* %arg) {
    entry:
      store i32 42, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %task = alloca i8* (i8*)*, align 8
      store i8* (i8*)* @task_body, i8* (i8*)** %task, align 8
      %task_raw = bitcast i8* (i8*)** %task to i8*
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %task_raw)
      %single = call i32 @__kmpc_single(i8* null, i32 0)
      call void @__kmpc_end_single(i8* null, i32 0)
      %after = load i32, i32* @shared, align 4
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *task_body = module->getFunction("task_body");
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(task_body, nullptr);
  ASSERT_NE(main_func, nullptr);

  const Instruction *task_store = &task_body->getEntryBlock().front();
  const Instruction *after = findInstructionByName(*main_func, "after");
  ASSERT_NE(task_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_TRUE(hb.happensBefore(task_store, after));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
