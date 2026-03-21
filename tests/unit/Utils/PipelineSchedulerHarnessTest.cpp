#include "Utils/Parallel/Scheduler/PipelineScheduler.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <gtest/gtest.h>

namespace {

std::unique_ptr<llvm::Module> parseAssembly(llvm::LLVMContext &Ctx,
                                            const char *IR) {
  llvm::SMDiagnostic Err;
  auto M = llvm::parseAssemblyString(IR, Err, Ctx);
  if (!M)
    Err.print("PipelineSchedulerHarnessTest", llvm::errs());
  return M;
}

TEST(PipelineSchedulerHarnessTest, TimeoutCancelsQueuedWorkAndRethrows) {
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
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_Local);
  Scheduler.setTaskTimeout(0);

  std::atomic<int> executed(0);
  Scheduler.setTaskCallback([&](const llvm::Function *) {
    executed.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  });

  EXPECT_THROW(Scheduler.run(), std::runtime_error);
  EXPECT_LT(executed.load(std::memory_order_relaxed), 3);
}

TEST(PipelineSchedulerHarnessTest, BottomUpStillSchedulesAllFunctions) {
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

  std::vector<std::string> visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    visited.push_back(F->getName().str());
  });

  Scheduler.run();

  ASSERT_EQ(visited.size(), 3u);
  EXPECT_EQ(visited[0], "leaf");
  EXPECT_EQ(visited[1], "mid");
  EXPECT_EQ(visited[2], "root");
}

} // namespace

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv,
                                    "PipelineScheduler harness\n");
  return RUN_ALL_TESTS();
}
