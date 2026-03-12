#include "Analysis/Concurrency/Utils/OpenMPTaskGraph.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace OpenMP;

class OpenMPTaskGraphTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("OpenMPTaskGraphTest", errs());
    }
    return module;
  }
};

TEST_F(OpenMPTaskGraphTest, ParsesStackBuiltDependencies) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %deps1 = alloca [1 x %kmp_depend_info], align 8
      %deps2 = alloca [1 x %kmp_depend_info], align 8

      %d10 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* %deps1, i64 0, i64 0, i32 0
      %d11 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* %deps1, i64 0, i64 0, i32 1
      %d12 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* %deps1, i64 0, i64 0, i32 2
      store i8* bitcast (i32* @shared to i8*), i8** %d10, align 8
      store i64 4, i64* %d11, align 8
      store i8 2, i8* %d12, align 1

      %d20 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* %deps2, i64 0, i64 0, i32 0
      %d21 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* %deps2, i64 0, i64 0, i32 1
      %d22 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* %deps2, i64 0, i64 0, i32 2
      store i8* bitcast (i32* @shared to i8*), i8** %d20, align 8
      store i64 4, i64* %d21, align 8
      store i8 1, i8* %d22, align 1

      %dep1 = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* %deps1, i64 0, i64 0
      %dep2 = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* %deps2, i64 0, i64 0

      %t1 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep1, i32 0, %kmp_depend_info* null)
      %t2 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep2, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_EQ(tasks[0]->dependencies.size(), 1u);
  EXPECT_EQ(tasks[1]->dependencies.size(), 1u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_FALSE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
}

TEST_F(OpenMPTaskGraphTest, DisjointOffsetsDoNotConflict) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @buffer = global [2 x i32] zeroinitializer, align 4
    @deps1 = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* getelementptr inbounds ([2 x i32], [2 x i32]* @buffer, i64 0, i64 0) to i8*),
        i64 4,
        i8 2
      }
    ]
    @deps2 = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* getelementptr inbounds ([2 x i32], [2 x i32]* @buffer, i64 0, i64 1) to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %dep1 = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* @deps1, i64 0, i64 0
      %dep2 = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* @deps2, i64 0, i64 0
      %t1 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep1, i32 0, %kmp_depend_info* null)
      %t2 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep2, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_TRUE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
