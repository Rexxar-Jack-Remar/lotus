#include "Utils/Parallel/Scheduler/PipelineScheduler.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <gtest/gtest.h>

namespace {

std::unique_ptr<llvm::Module> parseAssembly(llvm::LLVMContext &Ctx,
                                            const char *IR) {
  llvm::SMDiagnostic Err;
  auto M = llvm::parseAssemblyString(IR, Err, Ctx);
  if (!M)
    Err.print("PipelineSchedulerTest", llvm::errs());
  return M;
}

TEST(PipelineSchedulerTest, BottomUpSchedulingRespectsAcyclicDependencies) {
  static constexpr const char *IR = R"IR(
    define void @leaf() {
    entry:
      ret void
    }

    define void @mid() {
    entry:
      call void @leaf()
      ret void
    }

    define void @root() {
    entry:
      call void @mid()
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_BottomUp);

  std::vector<std::string> Visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    Visited.push_back(F->getName().str());
  });

  Scheduler.run();

  ASSERT_EQ(Visited.size(), 3u);
  EXPECT_EQ(Visited[0], "leaf");
  EXPECT_EQ(Visited[1], "mid");
  EXPECT_EQ(Visited[2], "root");
}

TEST(PipelineSchedulerTest, BottomUpSchedulingHandlesRecursiveCycles) {
  static constexpr const char *IR = R"IR(
    define void @entry() {
    entry:
      call void @a()
      ret void
    }

    define void @a() {
    entry:
      call void @b()
      ret void
    }

    define void @b() {
    entry:
      call void @a()
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_BottomUp);

  std::vector<std::string> Visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    Visited.push_back(F->getName().str());
  });

  Scheduler.run();

  ASSERT_EQ(Visited.size(), 3u);
  EXPECT_EQ(Visited[0], "a");
  EXPECT_EQ(Visited[1], "b");
  EXPECT_EQ(Visited[2], "entry");
}

TEST(PipelineSchedulerTest, TopDownSchedulingRespectsAcyclicDependencies) {
  static constexpr const char *IR = R"IR(
    define void @root() {
    entry:
      call void @mid()
      ret void
    }

    define void @mid() {
    entry:
      call void @leaf()
      ret void
    }

    define void @leaf() {
    entry:
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_TopDown);

  std::vector<std::string> Visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    Visited.push_back(F->getName().str());
  });

  Scheduler.run();

  ASSERT_EQ(Visited.size(), 3u);
  EXPECT_EQ(Visited[0], "root");
  EXPECT_EQ(Visited[1], "mid");
  EXPECT_EQ(Visited[2], "leaf");
}

TEST(PipelineSchedulerTest, BottomUpSchedulingHandlesMixedLeafAndRecursiveScc) {
  static constexpr const char *IR = R"IR(
    define void @leaf() {
    entry:
      ret void
    }

    define void @a() {
    entry:
      call void @b()
      ret void
    }

    define void @b() {
    entry:
      call void @a()
      ret void
    }

    define void @root() {
    entry:
      call void @leaf()
      call void @a()
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_BottomUp);

  std::vector<std::string> Visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    Visited.push_back(F->getName().str());
  });

  Scheduler.run();

  ASSERT_EQ(Visited.size(), 4u);
  const auto leaf_pos =
      std::find(Visited.begin(), Visited.end(), "leaf") - Visited.begin();
  const auto a_pos =
      std::find(Visited.begin(), Visited.end(), "a") - Visited.begin();
  const auto b_pos =
      std::find(Visited.begin(), Visited.end(), "b") - Visited.begin();
  const auto root_pos =
      std::find(Visited.begin(), Visited.end(), "root") - Visited.begin();

  ASSERT_LT(leaf_pos, Visited.size());
  ASSERT_LT(a_pos, Visited.size());
  ASSERT_LT(b_pos, Visited.size());
  ASSERT_LT(root_pos, Visited.size());
  EXPECT_LT(leaf_pos, root_pos);
  EXPECT_LT(a_pos, root_pos);
  EXPECT_LT(b_pos, root_pos);
  EXPECT_LT(a_pos, b_pos);
}

TEST(PipelineSchedulerTest, FlushesTrailingGarbageCollectionBatch) {
  static constexpr const char *IR = R"IR(
    define void @callee() {
    entry:
      ret void
    }

    define void @caller() {
    entry:
      call void @callee()
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_BottomUp);
  Scheduler.setGCBatchSize(1000);

  std::set<std::string> Released;
  Scheduler.setTaskCallback([](const llvm::Function *) {});
  Scheduler.setGCCallback([&](const llvm::Function *F) {
    Released.insert(F->getName().str());
  });

  Scheduler.run();

  EXPECT_EQ(Released, (std::set<std::string>{"callee", "caller"}));
}

} // namespace
