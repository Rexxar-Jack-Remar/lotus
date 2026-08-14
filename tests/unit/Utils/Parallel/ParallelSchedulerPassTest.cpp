#include "Utils/Parallel/Scheduler/ParallelSchedulerPass.h"

#include <gtest/gtest.h>

namespace {

TEST(ParallelSchedulerPassTest, AcceptsKnownAnalysisModes) {
  ParallelSchedulerPass Pass;

  EXPECT_NO_THROW(Pass.setAnalysisType(ParallelSchedulerPass::AM_Local));
  EXPECT_NO_THROW(Pass.setAnalysisType(ParallelSchedulerPass::AM_BottomUp));
  EXPECT_NO_THROW(Pass.setAnalysisType(ParallelSchedulerPass::AM_TopDown));
  EXPECT_NO_THROW(Pass.setAnalysisType(0));
  EXPECT_NO_THROW(Pass.setAnalysisType(1));
  EXPECT_NO_THROW(Pass.setAnalysisType(2));
}

TEST(ParallelSchedulerPassTest, RejectsInvalidIntegerAnalysisModes) {
  ParallelSchedulerPass Pass;

  EXPECT_DEATH(Pass.setAnalysisType(99), "invalid analysis type");
}

} // namespace
