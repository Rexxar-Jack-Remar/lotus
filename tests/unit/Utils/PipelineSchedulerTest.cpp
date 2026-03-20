#include "Utils/Parallel/Scheduler/PipelineScheduler.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

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
  Scheduler.setTaskTimeout(1);

  std::set<std::string> Visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    Visited.insert(F->getName().str());
  });

  Scheduler.run();

  EXPECT_EQ(Visited, (std::set<std::string>{"entry", "a", "b"}));
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
