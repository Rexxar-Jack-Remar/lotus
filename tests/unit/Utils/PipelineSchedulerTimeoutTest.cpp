#include "Utils/Parallel/Cancellation.h"
#include "Utils/Parallel/Scheduler/Task.h"
#include "Utils/Parallel/ThreadPool.h"
#include "Utils/Platform/ProgressBar.h"

#include <chrono>
#include <memory>
#include <string>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#define private public
#include "Utils/Parallel/Scheduler/PipelineScheduler.h"
#undef private

#include <gtest/gtest.h>

namespace {

std::unique_ptr<llvm::Module> parseAssembly(llvm::LLVMContext &Ctx,
                                            const char *IR) {
  llvm::SMDiagnostic Err;
  auto M = llvm::parseAssemblyString(IR, Err, Ctx);
  if (!M)
    Err.print("PipelineSchedulerTimeoutTest", llvm::errs());
  return M;
}

TEST(PipelineSchedulerTimeoutTest, WaitTaskUsesSecondBasedTimeouts) {
  static constexpr const char *IR = R"IR(
    define void @root() {
    entry:
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M = parseAssembly(Ctx, IR);
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_Local);
  Scheduler.setTaskTimeout(1);

  const auto Start = std::chrono::steady_clock::now();
  Scheduler.waitTask();
  const auto Elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - Start);

  EXPECT_GE(Elapsed, std::chrono::milliseconds(700));
  EXPECT_LT(Elapsed, std::chrono::milliseconds(1800));
  ASSERT_TRUE(Scheduler.getTaskFailure());

  try {
    std::rethrow_exception(Scheduler.getTaskFailure());
    FAIL() << "waitTask() should report a timeout";
  } catch (const std::runtime_error &Err) {
    EXPECT_NE(std::string(Err.what()).find("timed out"), std::string::npos);
  }
}

} // namespace
