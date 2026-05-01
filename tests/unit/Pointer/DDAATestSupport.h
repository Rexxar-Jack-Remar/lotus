#pragma once

/**
 * @file DDAATest.cpp
 * @brief Unit tests for SVF-style demand-driven analysis (DDA) on SVFG
 */

#include "Alias/DemandDriven/DDA/ContextDDA.h"
#include "Alias/DemandDriven/DDA/CxtDPItem.h"
#include "Alias/DemandDriven/DDA/DDAPass.h"
#include "Alias/DemandDriven/DDA/FlowDDA.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGNode.h"
#include "IR/SVFG/SVFGStats.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <set>

#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::unittest;

class DDAATest : public LlvmModuleTest {
protected:
  static bool pointsToSetContains(const std::vector<const Value *> &ptsSet,
                                  const Value *v) {
    return std::find(ptsSet.begin(), ptsSet.end(), v) != ptsSet.end();
  }
};

class FlowDDATestHelper : public FlowDDA {
public:
  using FlowDDA::onIndirectEdgesAdded;
};

class ContextDDATestHelper : public ContextDDA {
public:
  using ContextDDA::ContextDDA;
  using ContextDDA::isStrongUpdate;
};
