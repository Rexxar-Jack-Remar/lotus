#include "DDAATestSupport.h"

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
TEST_F(DDAATest, QueryLookupDoesNotClobberCanonicalObjectBinding) {
  const char *source = R"(
    define void @foo() {
      ret void
    }

    define void @bar() {
      %fp = alloca void ()*
      store void ()* bitcast (void ()* @foo to void ()*), void ()** %fp
      %x = load void ()*, void ()** %fp
      ret void
    }

    define i32 @main() {
      call void @bar()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  SVFG *svfg = flow.getSVFG();
  ASSERT_NE(svfg, nullptr);

  const Function *foo = module->getFunction("foo");
  const Function *bar = module->getFunction("bar");
  ASSERT_NE(foo, nullptr);
  ASSERT_NE(bar, nullptr);

  const LoadInst *x = nullptr;
  for (const BasicBlock &BB : *bar) {
    for (const Instruction &I : BB) {
      if (const auto *LI = dyn_cast<LoadInst>(&I))
        x = LI;
    }
  }
  ASSERT_NE(x, nullptr);

  SVFGNodeBS objIds = flow.getObjectIdsForValue(x);
  ASSERT_EQ(objIds.size(), 1u);
  const uint32_t objId = *objIds.begin();

  EXPECT_EQ(svfg->getObjectValue(objId), foo);
  EXPECT_EQ(svfg->getObjectId(foo), objId);
  EXPECT_EQ(svfg->getObjectId(x), 0u);
}
TEST_F(DDAATest, ExplicitQueryListDeduplicatesRepeatedPointers) {
  const char *source = R"(
    define void @test() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      ret void
    }

    define i32 @main() {
      call void @test()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  ASSERT_NE(flow.getSVFG(), nullptr);

  const Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);
  const AllocaInst *p = nullptr;
  for (const BasicBlock &BB : *F) {
    for (const Instruction &I : BB) {
      if (const auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isPointerTy())
          p = AI;
      }
    }
  }
  ASSERT_NE(p, nullptr);

  DDAClient client;
  client.setSVFG(flow.getSVFG());
  client.addQuery(p);
  client.addQuery(p);

  const auto &queries = client.collectCandidateQueries();
  ASSERT_EQ(queries.size(), 1u);
  EXPECT_EQ(queries.front(), p);
}
TEST_F(DDAATest, DefaultDDAClientCollectsValueAliasesFromSVFGValueMap) {
  const char *source = R"(
    define void @foo() {
      ret void
    }

    define void @bar() {
      %fp = alloca i8*
      store i8* bitcast (void ()* @foo to i8*), i8** %fp
      ret void
    }

    define i32 @main() {
      call void @bar()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  ASSERT_NE(flow.getSVFG(), nullptr);

  const Function *bar = module->getFunction("bar");
  ASSERT_NE(bar, nullptr);

  const StoreInst *store = nullptr;
  for (const BasicBlock &BB : *bar) {
    for (const Instruction &I : BB) {
      if (const auto *SI = dyn_cast<StoreInst>(&I))
        store = SI;
    }
  }
  ASSERT_NE(store, nullptr);

  const Value *bitcastExpr = store->getValueOperand();
  ASSERT_NE(bitcastExpr, nullptr);
  ASSERT_TRUE(isa<ConstantExpr>(bitcastExpr));

  DDAClient client;
  client.setSVFG(flow.getSVFG());
  const auto &queries = client.collectCandidateQueries();
  EXPECT_TRUE(std::find(queries.begin(), queries.end(), bitcastExpr) !=
              queries.end());
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

  // Sanity: address-taken alloca should have a non-empty points-to (it points
  // to itself).
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
TEST_F(DDAATest, ContextDDAKeepsGenericHeapStoresWeak) {
  const char *source = R"(
    define i32 @main() {
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  ContextDDATestHelper ctx(&flow, nullptr);
  ASSERT_TRUE(ctx.run(*module));

  ASSERT_NE(flow.getSVFG(), nullptr);
  constexpr uint32_t heapObjId = 424242;
  SVFG::ObjectInfo info;
  info.isHeap = true;
  flow.getSVFG()->setObjectInfo(heapObjId, info);
  EXPECT_TRUE(flow.getSVFG()->isHeapObject(heapObjId));

  CxtPtSet pts;
  pts.insert(CxtVar(ContextCond(), heapObjId));
  EXPECT_FALSE(ctx.isStrongUpdate(pts, nullptr));
}
TEST_F(DDAATest, ContextDDAAllowsConcreteHeapStrongUpdatesWhenSafe) {
  const char *source = R"(
    declare noalias i8* @malloc(i64)

    define i32 @main() {
      %h = call noalias i8* @malloc(i64 4)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  ContextDDATestHelper ctx(&flow, nullptr);
  ASSERT_TRUE(ctx.run(*module));
  ASSERT_NE(flow.getSVFG(), nullptr);

  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const CallInst *heapCall = nullptr;
  for (const BasicBlock &BB : *mainFn) {
    for (const Instruction &I : BB) {
      if (const auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getCalledFunction() &&
            CI->getCalledFunction()->getName() == "malloc") {
          heapCall = CI;
          break;
        }
      }
    }
  }
  ASSERT_NE(heapCall, nullptr);

  constexpr uint32_t heapObjId = 424243;
  SVFG::ObjectInfo info;
  info.isHeap = true;
  info.isConcreteHeap = true;
  info.baseObjId = heapObjId;
  flow.getSVFG()->setObjectInfo(heapObjId, info);
  flow.getSVFG()->setObjectValue(heapObjId, heapCall);
  EXPECT_TRUE(flow.getSVFG()->isHeapObject(heapObjId));
  EXPECT_TRUE(flow.getSVFG()->isConcreteHeapObject(heapObjId));

  CxtPtSet concretePts;
  concretePts.insert(CxtVar(ContextCond(), heapObjId));
  EXPECT_TRUE(ctx.isStrongUpdate(concretePts, nullptr));

  ContextCond nonConcrete;
  nonConcrete.setNonConcreteCxt();
  CxtPtSet nonConcretePts;
  nonConcretePts.insert(CxtVar(nonConcrete, heapObjId));
  EXPECT_FALSE(ctx.isStrongUpdate(nonConcretePts, nullptr));
}
TEST_F(DDAATest, FlowDDARecursionInfoSurvivesPartialIndirectRefinement) {
  const char *source = R"(
    define void @B(i8* %ptr) {
      ret void
    }

    define void @D(i8* %ptr) {
    entry:
      call void @A(i1 false, i8* %ptr)
      br label %exit
    exit:
      ret void
    }

    define void @A(i1 %choose_b, i8* %ptr) {
    entry:
      %fp = select i1 %choose_b, void (i8*)* @B, void (i8*)* @D
      call void %fp(i8* %ptr)
      ret void
    }

    define i32 @main() {
      %x = alloca i8
      call void @A(i1 true, i8* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDATestHelper flow;
  ASSERT_TRUE(flow.run(*module));
  ASSERT_NE(flow.getSVFG(), nullptr);
  ASSERT_NE(flow.getSVFGBuilder(), nullptr);

  const Function *A = module->getFunction("A");
  const Function *B = module->getFunction("B");
  const Function *D = module->getFunction("D");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(B, nullptr);
  ASSERT_NE(D, nullptr);
  EXPECT_TRUE(flow.isRecursiveFunction(A));
  EXPECT_TRUE(flow.isRecursiveFunction(D));

  const CallBase *indCall = nullptr;
  for (const BasicBlock &BB : *A) {
    for (const Instruction &I : BB) {
      if (const auto *CB = dyn_cast<CallBase>(&I)) {
        if (!CB->getCalledFunction()) {
          indCall = CB;
          break;
        }
      }
    }
    if (indCall)
      break;
  }
  ASSERT_NE(indCall, nullptr);

  std::vector<SVFGEdge *> newEdges;
  EXPECT_TRUE(flow.getSVFGBuilder()->connectCallSiteToCalleeOnTheFly(
      flow.getSVFG(), indCall, B, newEdges));
  EXPECT_FALSE(newEdges.empty());

  flow.onIndirectEdgesAdded();

  EXPECT_TRUE(flow.isRecursiveFunction(A));
  EXPECT_TRUE(flow.isRecursiveFunction(D));
}
TEST_F(DDAATest, CalleeEntryQueryRefinesMemoryNeutralIndirectCaller) {
  const char *source = R"(
    define void @target(i8* %p) {
    entry:
      ret void
    }

    define void @apply(void (i8*)* %fp, i8* %arg) {
    entry:
      call void %fp(i8* %arg)
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      call void @apply(void (i8*)* @target, i8* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  ASSERT_NE(flow.getSVFG(), nullptr);

  const Function *target = module->getFunction("target");
  const Function *apply = module->getFunction("apply");
  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(target, nullptr);
  ASSERT_NE(apply, nullptr);
  ASSERT_NE(mainFn, nullptr);

  const CallBase *indirectCall = nullptr;
  const AllocaInst *x = nullptr;
  for (const BasicBlock &BB : *apply) {
    for (const Instruction &I : BB) {
      if (const auto *CB = dyn_cast<CallBase>(&I)) {
        if (!CB->getCalledFunction())
          indirectCall = CB;
      }
    }
  }
  for (const BasicBlock &BB : *mainFn) {
    for (const Instruction &I : BB) {
      if (const auto *AI = dyn_cast<AllocaInst>(&I))
        x = AI;
    }
  }
  ASSERT_NE(indirectCall, nullptr);
  ASSERT_NE(x, nullptr);

  const Argument *formalArg =
      target->arg_empty() ? nullptr : &*target->arg_begin();
  ASSERT_NE(formalArg, nullptr);

  const auto &indCallers = flow.getSVFG()->getIndCallSitesInvokingCallee(target);
  EXPECT_TRUE(indCallers.count(indirectCall) != 0);

  std::vector<const Value *> ptsSet;
  FlowDDA::PtsSet rawPts = flow.getPointsTo(formalArg);
  ASSERT_FALSE(rawPts.empty());
  ASSERT_TRUE(flow.getPointsToSet(formalArg, ptsSet));
  EXPECT_TRUE(pointsToSetContains(ptsSet, x));
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
TEST_F(DDAATest, SVFGSCCHandlesDeepCyclesIteratively) {
  auto graph = std::make_unique<SVFG>();

  constexpr uint32_t kNodeCount = 20000;
  CopySVFGNode *first = nullptr;
  CopySVFGNode *prev = nullptr;
  CopySVFGNode *last = nullptr;
  for (uint32_t i = 1; i <= kNodeCount; ++i) {
    auto *node = new CopySVFGNode(i, nullptr, nullptr);
    graph->addNode(node);
    if (!first)
      first = node;
    if (prev)
      ASSERT_NE(graph->addEdge(prev, node, SVFGEdgeK::IntraCopy), nullptr);
    prev = node;
    last = node;
  }

  ASSERT_NE(first, nullptr);
  ASSERT_NE(last, nullptr);
  SVFGEdge *backEdge = graph->addEdge(last, first, SVFGEdgeK::IntraCopy);
  ASSERT_NE(backEdge, nullptr);

  SVFGStats stats(graph.get());
  stats.performSCCAnalysis({});

  EXPECT_EQ(stats.getNumSCCs(), 1u);
  EXPECT_EQ(stats.getSCCSize(first->getId()), kNodeCount);
  EXPECT_EQ(stats.getSCCRepNode(last->getId()), first->getId());
  EXPECT_TRUE(stats.isEdgeInSVFGSCC(backEdge));
}
TEST_F(DDAATest, RemoveNodeHandlesSelfLoopSafely) {
  auto graph = std::make_unique<SVFG>();
  auto *n1 = new CopySVFGNode(1, nullptr, nullptr);
  graph->addNode(n1);
  SVFGEdge *self = graph->addEdge(n1, n1, SVFGEdgeK::IntraCopy);
  ASSERT_NE(self, nullptr);

  graph->removeNode(n1);

  EXPECT_EQ(graph->getNumNodes(), 0u);
  const SVFGStat &stat = graph->getStat();
  EXPECT_EQ(stat.numNodes, 0u);
  EXPECT_EQ(stat.numEdges, 0u);
}
TEST_F(DDAATest, ContextSensitiveBKConditionOnCallAInRetAOut) {
  const char *source = R"(
    define void @setter(i32** %p, i32* %x) {
      %tmp = load i32*, i32** %p
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
      if (!callAInEdge && edge->getEdgeKind() == SVFGEdgeK::CallAIn &&
          isa<ActualInSVFGNode>(edge->getSrcNode()) &&
          isa<FormalInSVFGNode>(edge->getDstNode()))
        callAInEdge = edge;
      if (!retAOutEdge && edge->getEdgeKind() == SVFGEdgeK::RetAOut &&
          isa<FormalOutSVFGNode>(edge->getSrcNode()) &&
          isa<ActualOutSVFGNode>(edge->getDstNode()))
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

  // RetAOut: seeing same callsite already in context triggers OOB prune
  // (false).
  ContextCond retCond;
  EXPECT_TRUE(retCond.pushContext(retCsId));
  CxtLocDPItem retDpm(CxtVar(retCond, retAOutEdge->getSrcNode()->getId()),
                      retAOutEdge->getSrcNode());
  EXPECT_FALSE(ctx.handleBKCondition(retDpm, retAOutEdge));
  EXPECT_TRUE(ctx.isOutOfBudget());
}
TEST_F(DDAATest, IndirectCallRefinementPreservesIndirectEdgeKinds) {
  const char *source = R"(
    define i8* @callee(i8* %p) {
      ret i8* %p
    }

    define i8* @caller(i8* (i8*)* %fp, i8* %p) {
      %r = call i8* %fp(i8* %p)
      ret i8* %r
    }

    define i32 @main() {
      %x = alloca i8
      %r = call i8* @caller(i8* (i8*)* @callee, i8* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  SVFG *svfg = flow.getSVFG();
  ASSERT_NE(svfg, nullptr);
  ASSERT_NE(flow.getSVFGBuilder(), nullptr);

  const Function *caller = module->getFunction("caller");
  const Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  const CallBase *indCall = nullptr;
  for (const BasicBlock &BB : *caller) {
    for (const Instruction &I : BB) {
      if (const auto *CB = dyn_cast<CallBase>(&I)) {
        if (!CB->getCalledFunction()) {
          indCall = CB;
          break;
        }
      }
    }
  }
  ASSERT_NE(indCall, nullptr);

  std::vector<SVFGEdge *> newEdges;
  EXPECT_TRUE(flow.getSVFGBuilder()->connectCallSiteToCalleeOnTheFly(
      svfg, indCall, callee, newEdges));
  ASSERT_FALSE(newEdges.empty());

  bool sawCallInd = false;
  bool sawRetInd = false;
  for (SVFGEdge *edge : newEdges) {
    ASSERT_NE(edge, nullptr);
    if (edge->getEdgeKind() == SVFGEdgeK::CallInd)
      sawCallInd = true;
    if (edge->getEdgeKind() == SVFGEdgeK::RetInd)
      sawRetInd = true;
  }
  EXPECT_TRUE(sawCallInd);
  EXPECT_TRUE(sawRetInd);
}
TEST_F(DDAATest, IndirectCallRefinementMarksResolvedEdgesValid) {
  const char *source = R"(
    define i8* @foo(i8* %p) {
      ret i8* %p
    }

    define i8* @bar(i8* %p) {
      %fp = alloca i8* (i8*)*
      store i8* (i8*)* @foo, i8* (i8*)** %fp
      %x = load i8* (i8*)*, i8* (i8*)** %fp
      %r = call i8* %x(i8* %p)
      ret i8* %r
    }

    define i32 @main() {
      %x = alloca i8
      %r = call i8* @bar(i8* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  ASSERT_NE(flow.getSVFG(), nullptr);
  ASSERT_NE(flow.getSVFGBuilder(), nullptr);

  const Function *foo = module->getFunction("foo");
  const Function *bar = module->getFunction("bar");
  ASSERT_NE(foo, nullptr);
  ASSERT_NE(bar, nullptr);

  const LoadInst *x = nullptr;
  const CallBase *callResult = nullptr;
  const CallInst *indCall = nullptr;
  for (const BasicBlock &BB : *bar) {
    for (const Instruction &I : BB) {
      if (!x) {
        if (const auto *LI = dyn_cast<LoadInst>(&I))
          x = LI;
      }
      if (const auto *CI = dyn_cast<CallInst>(&I)) {
        if (!CI->getCalledFunction()) {
          indCall = CI;
          callResult = CI;
        }
      }
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(indCall, nullptr);
  ASSERT_NE(callResult, nullptr);

  (void)flow.getPointsTo(callResult);

  std::vector<SVFGEdge *> resolvedEdges;
  flow.getSVFG()->getInterVFEdgesForIndirectCallSite(indCall, foo,
                                                     resolvedEdges);
  ASSERT_FALSE(resolvedEdges.empty());
  for (SVFGEdge *edge : resolvedEdges) {
    ASSERT_NE(edge, nullptr);
    EXPECT_FALSE(flow.getSVFGBuilder()->isSpuriousVFEdgeAtIndCallSite(edge));
  }
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
    if (e->getDstNode() == varArg && (e->getEdgeKind() == SVFGEdgeK::CallDir ||
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
