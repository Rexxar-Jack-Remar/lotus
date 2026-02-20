/**
 * @file DDAATest.cpp
 * @brief Unit tests for SVF-style demand-driven analysis (DDA) on SVFG
 */

#include "Alias/DDA/FlowDDA.h"
#include "Alias/DDA/ContextDDA.h"
#include "Alias/DDA/CxtDPItem.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGNode.h"
#include "IR/SVFG/SVFGStats.h"

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

TEST_F(DDAATest, ContextCondUsesSlidingWindowWhenAtLimit) {
  ContextCond::setMaxCxtLen(3);
  ContextCond c;

  EXPECT_TRUE(c.pushContext(10));
  EXPECT_TRUE(c.pushContext(20));
  EXPECT_TRUE(c.pushContext(30));
  EXPECT_FALSE(c.pushContext(40));

  ASSERT_EQ(c.getContexts().size(), 3u);
  EXPECT_EQ(c.getContexts()[0], 20u);
  EXPECT_EQ(c.getContexts()[1], 30u);
  EXPECT_EQ(c.getContexts()[2], 40u);
  EXPECT_FALSE(c.isConcreteCxt());

  EXPECT_TRUE(c.matchContext(40));
  EXPECT_FALSE(c.matchContext(10));
}

TEST_F(DDAATest, SVFGSCCSkipsInsensitiveCallRetEdges) {
  auto graph = std::make_unique<SVFG>();
  auto *n1 = new CopySVFGNode(1, nullptr, nullptr);
  auto *n2 = new CopySVFGNode(2, nullptr, nullptr);
  graph->addNode(n1);
  graph->addNode(n2);

  SVFGEdge *callEdge = graph->addEdge(n1, n2, SVFGEdgeK::CallDir);
  SVFGEdge *retEdge = graph->addEdge(n2, n1, SVFGEdgeK::RetDir);
  ASSERT_NE(callEdge, nullptr);
  ASSERT_NE(retEdge, nullptr);

  SVFGStats stats(graph.get());
  stats.performSCCAnalysis({});
  EXPECT_TRUE(stats.isEdgeInSVFGSCC(callEdge));

  SVFGStats::SVFGEdgeSet insensitive{retEdge};
  stats.performSCCAnalysis(insensitive);
  EXPECT_FALSE(stats.isEdgeInSVFGSCC(callEdge));
}

TEST_F(DDAATest, ContextSensitiveBKConditionOnCallAInRetAOut) {
  const char *source = R"(
    define void @setter(i32** %p, i32* %x) {
      store i32* %x, i32** %p
      ret void
    }

    define void @apply(i32** %p, i32* %x, void (i32**, i32*)* %fp, i1 %rec) {
    entry:
      call void %fp(i32** %p, i32* %x)
      br i1 %rec, label %recur, label %exit
    recur:
      call void @apply(i32** %p, i32* %x, void (i32**, i32*)* %fp, i1 false)
      br label %exit
    exit:
      ret void
    }

    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      call void @apply(i32** %p, i32* %x, void (i32**, i32*)* @setter, i1 true)
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  ContextDDA ctx(&flow, nullptr);
  ASSERT_TRUE(ctx.run(*module));

  const Function *applyF = module->getFunction("apply");
  const Function *mainF = module->getFunction("main");
  ASSERT_NE(applyF, nullptr);
  ASSERT_NE(mainF, nullptr);
  const CallBase *indCall = nullptr;
  const LoadInst *q = nullptr;
  for (const BasicBlock &BB : *applyF) {
    for (const Instruction &I : BB) {
      if (!indCall) {
        if (const auto *CB = dyn_cast<CallBase>(&I))
          if (!CB->getCalledFunction())
            indCall = CB;
      }
    }
  }
  for (const BasicBlock &BB : *mainF) {
    for (const Instruction &I : BB) {
      if (const auto *LI = dyn_cast<LoadInst>(&I))
        q = LI;
    }
  }
  ASSERT_NE(indCall, nullptr);
  ASSERT_NE(q, nullptr);

  // Force on-the-fly indirect call refinement so CallAIn/RetAOut edges exist.
  auto *setter = module->getFunction("setter");
  ASSERT_NE(setter, nullptr);
  std::vector<SVFGEdge *> newEdges;
  ASSERT_NE(flow.getSVFGBuilder(), nullptr);
  ASSERT_NE(flow.getSVFG(), nullptr);
  EXPECT_TRUE(flow.getSVFGBuilder()->connectCallSiteToCalleeOnTheFly(
      flow.getSVFG(), indCall, setter, newEdges));
  EXPECT_FALSE(newEdges.empty());
  ctx.initInsensitiveEdges();
  (void)ctx.computeDDAPts(q);

  SVFG *svfg = ctx.getSVFG();
  ASSERT_NE(svfg, nullptr);
  SVFGEdge *callAInEdge = nullptr;
  SVFGEdge *retAOutEdge = nullptr;
  for (const auto &pair : *svfg) {
    SVFGNode *node = pair.second;
    if (!node)
      continue;
    for (SVFGEdge *edge : node->getOutEdges()) {
      if (!edge || edge->getCallSite() != indCall)
        continue;
      if (!callAInEdge && edge->getEdgeKind() == SVFGEdgeK::CallAIn)
        callAInEdge = edge;
      if (!retAOutEdge && edge->getEdgeKind() == SVFGEdgeK::RetAOut)
        retAOutEdge = edge;
    }
  }

  ASSERT_NE(callAInEdge, nullptr);
  ASSERT_NE(retAOutEdge, nullptr);

  CxtLocDPItem dummyCall(CxtVar(ContextCond(), 0), callAInEdge->getSrcNode());
  CxtLocDPItem dummyRet(CxtVar(ContextCond(), 0), retAOutEdge->getSrcNode());
  uint32_t callCsId = ctx.getCSIDAtCall(dummyCall, callAInEdge);
  uint32_t retCsId = ctx.getCSIDAtRet(dummyRet, retAOutEdge);
  ASSERT_NE(callCsId, 0u);
  ASSERT_NE(retCsId, 0u);

  // CallAIn: mismatched call string must be pruned (false).
  ContextCond callCond;
  EXPECT_TRUE(callCond.pushContext(callCsId + 1));
  CxtLocDPItem callDpm(CxtVar(callCond, callAInEdge->getSrcNode()->getId()),
                       callAInEdge->getSrcNode());
  EXPECT_FALSE(ctx.handleBKCondition(callDpm, callAInEdge));

  // RetAOut: seeing same callsite already in context triggers OOB prune (false).
  ContextCond retCond;
  EXPECT_TRUE(retCond.pushContext(retCsId));
  CxtLocDPItem retDpm(CxtVar(retCond, retAOutEdge->getSrcNode()->getId()),
                      retAOutEdge->getSrcNode());
  EXPECT_FALSE(ctx.handleBKCondition(retDpm, retAOutEdge));
  EXPECT_TRUE(ctx.isOutOfBudget());
}

TEST_F(DDAATest, HandlesVarArgValueFlowNodes) {
  const char *source = R"(
    define void @sink(i8* %a, ...) {
      ret void
    }

    define i32 @main() {
      %x = alloca i8
      call void (i8*, ...) @sink(i8* %x, i8* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  SVFG *svfg = flow.getSVFG();
  ASSERT_NE(svfg, nullptr);

  const Function *sink = module->getFunction("sink");
  ASSERT_NE(sink, nullptr);
  const CallBase *cs = nullptr;
  for (const BasicBlock &BB : *module->getFunction("main")) {
    for (const Instruction &I : BB) {
      if (const auto *CB = dyn_cast<CallBase>(&I))
        cs = CB;
    }
  }
  ASSERT_NE(cs, nullptr);

  const VarArgSVFGNode *varArg = nullptr;
  const ActualParmSVFGNode *extraArg = nullptr;
  for (const auto &pair : *svfg) {
    const SVFGNode *n = pair.second;
    if (!n)
      continue;
    if (!varArg) {
      if (const auto *v = dyn_cast<VarArgSVFGNode>(n)) {
        if (v->getFunction() == sink)
          varArg = v;
      }
    }
    if (!extraArg) {
      if (const auto *ap = dyn_cast<ActualParmSVFGNode>(n)) {
        if (ap->getCallSite() == cs && ap->getParamIndex() == 1)
          extraArg = ap;
      }
    }
  }
  ASSERT_NE(varArg, nullptr);
  ASSERT_NE(extraArg, nullptr);

  bool connected = false;
  for (const SVFGEdge *e : extraArg->getOutEdges()) {
    if (!e)
      continue;
    if (e->getDstNode() == varArg &&
        (e->getEdgeKind() == SVFGEdgeK::CallDir ||
         e->getEdgeKind() == SVFGEdgeK::CallInd)) {
      connected = true;
      break;
    }
  }
  EXPECT_TRUE(connected);
}

TEST_F(DDAATest, MarksConstantGlobalObjects) {
  const char *source = R"(
    @g = constant i32 7
    @p = constant i32* @g

    define i32 @main() {
      %q = load i32*, i32** @p
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  SVFG *svfg = flow.getSVFG();
  ASSERT_NE(svfg, nullptr);

  const GlobalVariable *p = module->getNamedGlobal("p");
  ASSERT_NE(p, nullptr);
  SVFGNodeBS ids = flow.getObjectIdsForValue(p);
  ASSERT_FALSE(ids.empty());
  bool seenConstant = false;
  for (uint32_t id : ids) {
    if (svfg->isConstantObject(id)) {
      seenConstant = true;
      break;
    }
  }
  EXPECT_TRUE(seenConstant);
}

TEST_F(DDAATest, OutOfBudgetFallbackIsConservativeAndNonEmpty) {
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

  uint32_t oldBudget = FlowDDA::getDefaultMaxBudget();
  FlowDDA::setDefaultMaxBudget(0);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  const Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);
  const LoadInst *q = nullptr;
  for (const BasicBlock &BB : *F) {
    for (const Instruction &I : BB) {
      if (const auto *LI = dyn_cast<LoadInst>(&I))
        q = LI;
    }
  }
  ASSERT_NE(q, nullptr);

  FlowDDA::PtsSet pts = flow.getPointsTo(q);
  EXPECT_FALSE(pts.empty());
  ASSERT_NE(flow.getStat(), nullptr);
  EXPECT_GE(flow.getStat()->numOutOfBudgetQueries, 1u);

  FlowDDA::setDefaultMaxBudget(oldBudget);
}
