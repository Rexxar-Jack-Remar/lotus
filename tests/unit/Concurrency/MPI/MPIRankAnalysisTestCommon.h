#pragma once

#include "Concurrency/MPI/MPIRankAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace MPI;
using namespace lotus::unittest;

class MPIRankAnalysisTest : public LlvmModuleTest {
protected:
  using LlvmModuleTest::parseModule;
};