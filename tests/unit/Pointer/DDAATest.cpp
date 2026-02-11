/**
 * @file DDAATest.cpp
 * @brief Unit tests for SVF-style demand-driven analysis (DDA) on SVFG
 */

#include "Alias/DDA/DemandDrivenAA.h"

#include <algorithm>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;

class DDAATest : public ::testing::Test {
protected:
  LLVMContext context;
  std::unique_ptr<Module> parseModule(const char *source) {
    SMDiagnostic err;
    auto module = parseAssemblyString(source, err, context);
    if (!module) {
      err.print("DDAATest", errs());
    }
    return module;
  }

  static bool pointsToSetContains(const std::vector<const Value *> &ptsSet,
                                  const Value *v) {
    return std::find(ptsSet.begin(), ptsSet.end(), v) != ptsSet.end();
  }
};

TEST_F(DDAATest, ResolvesFunctionPointerFromConstant) {
  const char *source = R"(
    define void @foo() {
      ret void
    }

    define void @bar() {
      %fp = alloca void ()*
      store void ()* bitcast (void ()* @foo to void ()*), void ()** %fp
      %x = load void ()*, void ()** %fp
      call void %x()
      ret void
    }

    define i32 @main() {
      call void @bar()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DemandDrivenAA dda;
  ASSERT_TRUE(dda.run(*module));

  Function *foo = module->getFunction("foo");
  Function *bar = module->getFunction("bar");
  ASSERT_NE(foo, nullptr);
  ASSERT_NE(bar, nullptr);

  const LoadInst *x = nullptr;
  const CallInst *indCall = nullptr;
  for (const BasicBlock &BB : *bar) {
    for (const Instruction &I : BB) {
      if (!x) {
        if (const auto *LI = dyn_cast<LoadInst>(&I))
          x = LI;
      }
      if (const auto *CI = dyn_cast<CallInst>(&I)) {
        if (!CI->getCalledFunction())
          indCall = CI;
      }
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(indCall, nullptr);

  std::vector<const Value *> ptsSet;
  DemandDrivenAA::PtsSet rawPts = dda.getPointsTo(x);
  ASSERT_FALSE(rawPts.empty());
  ASSERT_TRUE(dda.getPointsToSet(x, ptsSet));
  EXPECT_TRUE(pointsToSetContains(ptsSet, foo));

  // On-the-fly indirect-call refinement should connect this callsite to @foo.
  ASSERT_NE(dda.getSVFGConst(), nullptr);
  EXPECT_TRUE(dda.getSVFGConst()->hasConnectedCallee(indCall, foo));
}

TEST_F(DDAATest, PropagatesThroughMemoryViaSVFGIndirectEdges) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret void
    }

    define i32 @main() {
      call void @test()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  DemandDrivenAA dda;
  ASSERT_TRUE(dda.run(*module));

  Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);

  const Value *x = nullptr;
  const LoadInst *q = nullptr;
  for (const BasicBlock &BB : *F) {
    for (const Instruction &I : BB) {
      if (const auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32) && !x)
          x = AI;
      }
      if (const auto *LI = dyn_cast<LoadInst>(&I))
        q = LI;
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(q, nullptr);

  // Sanity: address-taken alloca should have a non-empty points-to (it points to itself).
  {
    DemandDrivenAA::PtsSet xPts = dda.getPointsTo(x);
    ASSERT_FALSE(xPts.empty());
  }

  // SVF-design(A) invariant: Load statement node has guarded indirect edges
  // from reaching memory definitions (MemorySSA).
  ASSERT_NE(dda.getSVFG(), nullptr);
  const SVFGNode *loadNode = dda.getSVFG()->getDef(q);
  ASSERT_NE(loadNode, nullptr);
  bool hasGuardedIntraIndirect = false;
  for (const auto *edge : loadNode->getInEdges()) {
    if (!edge)
      continue;
    if (edge->getEdgeKind() == SVFGEdgeK::IntraIndirect &&
        !edge->getPointsTo().empty()) {
      hasGuardedIntraIndirect = true;
      break;
    }
  }
  EXPECT_TRUE(hasGuardedIntraIndirect);

  std::vector<const Value *> ptsSet;
  DemandDrivenAA::PtsSet rawPts = dda.getPointsTo(q);
  ASSERT_FALSE(rawPts.empty());
  ASSERT_TRUE(dda.getPointsToSet(q, ptsSet));
  EXPECT_TRUE(pointsToSetContains(ptsSet, x));
}
