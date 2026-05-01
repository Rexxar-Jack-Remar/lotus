#include "DDAATestSupport.h"

TEST_F(DDAATest, CanonicalObjectIdsMatchAddrNodesAndBuilderQueries) {
  const char *source = R"(
    %S = type { i32, i32 }

    @g = global i8 0

    declare noalias i8* @malloc(i64)

    define void @callee() {
      ret void
    }

    define void @test() {
      %a = alloca i8
      %s = alloca %S
      %f0 = getelementptr inbounds %S, %S* %s, i32 0, i32 0
      %h = call i8* @malloc(i64 4)
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
  SVFG *svfg = flow.getSVFG();
  ASSERT_NE(svfg, nullptr);

  const Function *testFn = module->getFunction("test");
  const Function *calleeFn = module->getFunction("callee");
  const GlobalVariable *g = module->getNamedGlobal("g");
  ASSERT_NE(testFn, nullptr);
  ASSERT_NE(calleeFn, nullptr);
  ASSERT_NE(g, nullptr);

  const AllocaInst *allocaA = nullptr;
  const AllocaInst *allocaS = nullptr;
  const GetElementPtrInst *fieldGep = nullptr;
  const CallInst *heapCall = nullptr;
  for (const BasicBlock &BB : *testFn) {
    for (const Instruction &I : BB) {
      if (const auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (!allocaA)
          allocaA = AI;
        else
          allocaS = AI;
      } else if (const auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
        fieldGep = GEP;
      } else if (const auto *CI = dyn_cast<CallInst>(&I)) {
        if (!CI->getCalledFunction() ||
            CI->getCalledFunction()->getName() == "malloc") {
          heapCall = CI;
        }
      }
    }
  }
  ASSERT_NE(allocaA, nullptr);
  ASSERT_NE(allocaS, nullptr);
  ASSERT_NE(fieldGep, nullptr);
  ASSERT_NE(heapCall, nullptr);

  auto getSingletonId = [&](const Value *value) -> uint32_t {
    SVFGNodeBS ids = flow.getObjectIdsForValue(value);
    EXPECT_EQ(ids.size(), 1u);
    if (ids.size() != 1u)
      return 0;
    return *ids.begin();
  };

  const uint32_t allocaObjId = getSingletonId(allocaA);
  const uint32_t globalObjId = getSingletonId(g);
  const uint32_t funObjId = getSingletonId(calleeFn);
  const uint32_t heapObjId = getSingletonId(heapCall);
  const uint32_t fieldObjId = getSingletonId(fieldGep);

  auto *allocaAddr = dyn_cast_or_null<AddrSVFGNode>(svfg->getValueNode(allocaA));
  ASSERT_NE(allocaAddr, nullptr);

  EXPECT_EQ(allocaAddr->getObjectId(), allocaObjId);
  EXPECT_EQ(svfg->getObjectId(allocaA), allocaObjId);
  EXPECT_EQ(svfg->getObjectId(g), globalObjId);
  EXPECT_EQ(svfg->getObjectId(calleeFn), funObjId);
  EXPECT_EQ(svfg->getObjectValue(globalObjId), g);
  EXPECT_EQ(svfg->getObjectValue(funObjId), calleeFn);
  EXPECT_NE(heapObjId, 0u);
  EXPECT_NE(fieldObjId, 0u);
}
TEST_F(DDAATest, DDAIdLookupReturnsCanonicalDefNode) {
  const char *source = R"(
    define i8* @id(i8* %p) {
      ret i8* %p
    }

    define i32 @main() {
      %x = alloca i8
      %r = call i8* @id(i8* %x)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  ContextDDA ctx(&flow, nullptr);
  ASSERT_TRUE(ctx.run(*module));

  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const CallBase *call = nullptr;
  for (const BasicBlock &BB : *mainFn) {
    for (const Instruction &I : BB) {
      if (const auto *CB = dyn_cast<CallBase>(&I))
        call = CB;
    }
  }
  ASSERT_NE(call, nullptr);
  ASSERT_NE(flow.getSVFG(), nullptr);

  SVFGNode *callValueNode = flow.getSVFG()->getValueNode(call);
  ASSERT_NE(callValueNode, nullptr);
  ASSERT_TRUE(callValueNode->hasValueId());

  const ActualRetSVFGNode *actualRet = nullptr;
  for (SVFGNode *node : flow.getSVFG()->getActualRets(call)) {
    actualRet = dyn_cast<ActualRetSVFGNode>(node);
    if (actualRet)
      break;
  }
  ASSERT_NE(actualRet, nullptr);
  EXPECT_EQ(actualRet->getValueId(), callValueNode->getValueId());
  EXPECT_EQ(flow.getSVFG()->getCanonicalDefNodeForDDAId(
                callValueNode->getValueId()),
            callValueNode);

  ContextCond cond;
  const CxtPtSet &byId =
      ctx.computeDDAPts(CxtVar(cond, callValueNode->getValueId()));
  const CxtPtSet &byValue = ctx.computeDDAPts(call);
  EXPECT_EQ(byId, byValue);

  const AllocaInst *x = nullptr;
  for (const BasicBlock &BB : *mainFn) {
    for (const Instruction &I : BB) {
      if (const auto *AI = dyn_cast<AllocaInst>(&I)) {
        x = AI;
        break;
      }
    }
    if (x)
      break;
  }
  ASSERT_NE(x, nullptr);

  SVFGNodeBS objIds = flow.getObjectIdsForValue(x);
  ASSERT_EQ(objIds.size(), 1u);
  const uint32_t objId = *objIds.begin();
  EXPECT_EQ(flow.getSVFG()->getCanonicalDefNodeForDDAId(objId),
            flow.getSVFG()->getValueNode(x));
}
TEST_F(DDAATest, ContextDDADefaultDoesNotEnableInsensitiveCycleHandling) {
  const char *source = R"(
    define void @target(i8** %pp, i8* %x) {
      store i8* %x, i8** %pp
      ret void
    }

    define void @recur(i8** %pp, i8* %x, void (i8**, i8*)* %fp, i1 %again) {
    entry:
      call void %fp(i8** %pp, i8* %x)
      br i1 %again, label %loop, label %exit
    loop:
      call void @recur(i8** %pp, i8* %x, void (i8**, i8*)* %fp, i1 false)
      br label %exit
    exit:
      ret void
    }

    define i32 @main() {
      %x = alloca i8
      %p = alloca i8*
      call void @recur(i8** %p, i8* %x, void (i8**, i8*)* @target, i1 true)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  ContextDDA ctx(&flow, nullptr);
  ASSERT_TRUE(ctx.run(*module));

  const Function *recurF = module->getFunction("recur");
  const Function *targetF = module->getFunction("target");
  ASSERT_NE(recurF, nullptr);
  ASSERT_NE(targetF, nullptr);

  const CallBase *recursiveCall = nullptr;
  for (const BasicBlock &BB : *recurF) {
    for (const Instruction &I : BB) {
      if (const auto *CB = dyn_cast<CallBase>(&I)) {
        if (CB->getCalledFunction() == recurF) {
          recursiveCall = CB;
          break;
        }
      }
    }
  }
  ASSERT_NE(recursiveCall, nullptr);
  ASSERT_NE(flow.getSVFG(), nullptr);

  SVFGEdge *callEdge = nullptr;
  for (const auto &pair : *flow.getSVFG()) {
    SVFGNode *node = pair.second;
    if (!node)
      continue;
    for (SVFGEdge *edge : node->getOutEdges()) {
      if (!edge || edge->getCallSite() != recursiveCall)
        continue;
      if (edge->getEdgeKind() == SVFGEdgeK::CallDir ||
          edge->getEdgeKind() == SVFGEdgeK::CallInd) {
        callEdge = edge;
        break;
      }
    }
    if (callEdge)
      break;
  }
  ASSERT_NE(callEdge, nullptr);

  CxtLocDPItem probe(CxtVar(ContextCond(), 0), callEdge->getSrcNode());
  const uint32_t csId = ctx.getCSIDAtCall(probe, callEdge);
  ASSERT_NE(csId, 0u);

  ContextCond mismatch;
  EXPECT_TRUE(mismatch.pushContext(csId + 1));
  CxtLocDPItem dpm(CxtVar(mismatch, callEdge->getSrcNode()->getId()),
                   callEdge->getSrcNode());

  // With SVF-compatible defaults, no additional insensitive-edge set should be
  // materialized during run().
  EXPECT_TRUE(ctx.getInsensitiveEdgeSet().empty());
  EXPECT_TRUE(ctx.handleBKCondition(dpm, callEdge));

  ContextDDA insensitiveCtx(&flow, nullptr);
  insensitiveCtx.setInsensitiveRecursion(true);
  ASSERT_TRUE(insensitiveCtx.run(*module));
  insensitiveCtx.initInsensitiveEdges();
  EXPECT_FALSE(insensitiveCtx.getInsensitiveEdgeSet().empty());
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
TEST_F(DDAATest, ContextOutOfBudgetFallbackMatchesFlowDDA) {
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

  const uint32_t oldBudget = FlowDDA::getDefaultMaxBudget();
  FlowDDA::setDefaultMaxBudget(0);

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

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  ContextDDA ctx(&flow, nullptr);
  ASSERT_TRUE(ctx.run(*module));

  FlowDDA::PtsSet flowPts = flow.getPointsTo(q);
  const CxtPtSet &cxtPts = ctx.computeDDAPts(q);

  ASSERT_FALSE(flowPts.empty());
  ASSERT_FALSE(cxtPts.empty());
  ASSERT_NE(ctx.getDDAStat(), nullptr);
  EXPECT_GE(ctx.getDDAStat()->numOutOfBudgetQueries, 1u);

  std::set<uint32_t> cxtObjIds;
  for (const CxtVar &var : cxtPts) {
    EXPECT_TRUE(var.get_cond().getContexts().empty());
    cxtObjIds.insert(var.get_id());
  }

  std::set<uint32_t> flowObjIds(flowPts.begin(), flowPts.end());
  EXPECT_EQ(cxtObjIds, flowObjIds);

  FlowDDA::setDefaultMaxBudget(oldBudget);
}
TEST_F(DDAATest, FieldSensitiveGepQueriesDoNotAliasDistinctStructFields) {
  const char *source = R"(
    %S = type { i32, i32 }

    define void @test() {
      %s = alloca %S
      %p = getelementptr inbounds %S, %S* %s, i32 0, i32 0
      %q = getelementptr inbounds %S, %S* %s, i32 0, i32 1
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

  const Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);
  const GetElementPtrInst *p = nullptr;
  const GetElementPtrInst *q = nullptr;
  for (const BasicBlock &BB : *F) {
    for (const Instruction &I : BB) {
      const auto *GEP = dyn_cast<GetElementPtrInst>(&I);
      if (!GEP)
        continue;
      if (!p)
        p = GEP;
      else
        q = GEP;
    }
  }
  ASSERT_NE(p, nullptr);
  ASSERT_NE(q, nullptr);

  EXPECT_FALSE(flow.mayAlias(p, q));
}
TEST_F(DDAATest, VariantGepQueriesProduceCanonicalFIObjects) {
  const char *source = R"(
    %S = type { [4 x i32] }

    define void @test(i64 %idx) {
      %s = alloca %S
      %fixed = getelementptr inbounds %S, %S* %s, i32 0, i32 0, i64 0
      %var = getelementptr inbounds %S, %S* %s, i32 0, i32 0, i64 %idx
      ret void
    }

    define i32 @main() {
      call void @test(i64 1)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  ASSERT_NE(flow.getSVFGConst(), nullptr);

  const Function *F = module->getFunction("test");
  ASSERT_NE(F, nullptr);
  const GetElementPtrInst *fixed = nullptr;
  const GetElementPtrInst *var = nullptr;
  for (const BasicBlock &BB : *F) {
    for (const Instruction &I : BB) {
      const auto *GEP = dyn_cast<GetElementPtrInst>(&I);
      if (!GEP)
        continue;
      if (GEP->hasAllConstantIndices())
        fixed = GEP;
      else
        var = GEP;
    }
  }
  ASSERT_NE(fixed, nullptr);
  ASSERT_NE(var, nullptr);

  FlowDDA::PtsSet varPts = flow.getPointsTo(var);
  ASSERT_EQ(varPts.size(), 1u);
  const uint32_t fiObjId = *varPts.begin();
  EXPECT_TRUE(flow.getSVFGConst()->isFieldInsensitiveObject(fiObjId));
  ASSERT_NE(flow.getSVFGBuilder(), nullptr);
  EXPECT_EQ(flow.getSVFGBuilder()->getOrCreateFIObjId(fiObjId), fiObjId);

  FlowDDA::PtsSet varPtsAgain = flow.getPointsTo(var);
  EXPECT_EQ(varPtsAgain, varPts);
}
TEST_F(DDAATest, ResolvesIndirectCallMemoryEffectsThroughFormalParmLoads) {
  const char *source = R"(
    @x = global i32 0

    define void @setp(i32** %p) {
      store i32* @x, i32** %p
      ret void
    }

    define void @caller(i32** %p, void (i32**)* %fp) {
      call void %fp(i32** %p)
      %q = load i32*, i32** %p
      ret void
    }

    define i32 @main() {
      %p = alloca i32*
      call void @caller(i32** %p, void (i32**)* @setp)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));

  const Function *caller = module->getFunction("caller");
  const Function *setp = module->getFunction("setp");
  const GlobalVariable *x = module->getNamedGlobal("x");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(setp, nullptr);
  ASSERT_NE(x, nullptr);

  const CallBase *indCall = nullptr;
  const LoadInst *q = nullptr;
  for (const BasicBlock &BB : *caller) {
    for (const Instruction &I : BB) {
      if (const auto *CB = dyn_cast<CallBase>(&I))
        if (!CB->getCalledFunction())
          indCall = CB;
      if (const auto *LI = dyn_cast<LoadInst>(&I))
        q = LI;
    }
  }
  ASSERT_NE(indCall, nullptr);
  ASSERT_NE(q, nullptr);

  std::vector<const Value *> ptsSet;
  FlowDDA::PtsSet rawPts = flow.getPointsTo(q);
  ASSERT_FALSE(rawPts.empty());
  ASSERT_TRUE(flow.getPointsToSet(q, ptsSet));
  EXPECT_TRUE(pointsToSetContains(ptsSet, x));

  ASSERT_NE(flow.getSVFGConst(), nullptr);
  EXPECT_TRUE(flow.getSVFGConst()->hasConnectedCallee(indCall, setp));
}
TEST_F(DDAATest, SeedsPotentialIndirectCallerIndexBeforeEdgeMaterialization) {
  const char *source = R"(
    @x = global i32 0

    define void @setp(i32** %p) {
      store i32* @x, i32** %p
      ret void
    }

    define void @caller1(i32** %p, void (i32**)* %fp) {
      call void %fp(i32** %p)
      ret void
    }

    define void @caller2(i32** %p, void (i32**)* %fp) {
      call void %fp(i32** %p)
      ret void
    }

    define i32 @main() {
      %p = alloca i32*
      call void @caller1(i32** %p, void (i32**)* @setp)
      call void @caller2(i32** %p, void (i32**)* @setp)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  const SVFG *svfg = flow.getSVFGConst();
  ASSERT_NE(svfg, nullptr);

  const Function *setp = module->getFunction("setp");
  const Function *caller1 = module->getFunction("caller1");
  const Function *caller2 = module->getFunction("caller2");
  ASSERT_NE(setp, nullptr);
  ASSERT_NE(caller1, nullptr);
  ASSERT_NE(caller2, nullptr);

  const CallBase *indCall1 = nullptr;
  const CallBase *indCall2 = nullptr;
  for (const BasicBlock &BB : *caller1) {
    for (const Instruction &I : BB) {
      if (const auto *CB = dyn_cast<CallBase>(&I))
        if (!CB->getCalledFunction())
          indCall1 = CB;
    }
  }
  for (const BasicBlock &BB : *caller2) {
    for (const Instruction &I : BB) {
      if (const auto *CB = dyn_cast<CallBase>(&I))
        if (!CB->getCalledFunction())
          indCall2 = CB;
    }
  }
  ASSERT_NE(indCall1, nullptr);
  ASSERT_NE(indCall2, nullptr);

  const auto &callers = svfg->getIndCallSitesInvokingCallee(setp);
  EXPECT_EQ(callers.count(indCall1), 1u);
  EXPECT_EQ(callers.count(indCall2), 1u);
  EXPECT_FALSE(svfg->hasConnectedCallee(indCall1, setp));
  EXPECT_FALSE(svfg->hasConnectedCallee(indCall2, setp));
}
TEST_F(DDAATest, IndirectCallIndexUsesStableTopLevelValueIds) {
  const char *source = R"(
    define void @sink(i32** %p) {
      ret void
    }

    define void @caller(i32** %p, void (i32**)* %fp) {
      call void %fp(i32** %p)
      ret void
    }

    define i32 @main() {
      %p = alloca i32*
      call void @caller(i32** %p, void (i32**)* @sink)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  FlowDDA flow;
  ASSERT_TRUE(flow.run(*module));
  SVFG *svfg = flow.getSVFG();
  ASSERT_NE(svfg, nullptr);

  const Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  const CallBase *indCall = nullptr;
  for (const BasicBlock &BB : *caller) {
    for (const Instruction &I : BB) {
      if (const auto *CB = dyn_cast<CallBase>(&I))
        if (!CB->getCalledFunction())
          indCall = CB;
    }
  }
  ASSERT_NE(indCall, nullptr);

  const FormalParmSVFGNode *formalFp = nullptr;
  for (SVFGNode *node : svfg->getFormalParms(caller)) {
    auto *formalParm = dyn_cast<FormalParmSVFGNode>(node);
    if (!formalParm || formalParm->getParamIndex() != 1)
      continue;
    formalFp = formalParm;
    break;
  }
  ASSERT_NE(formalFp, nullptr);
  ASSERT_TRUE(formalFp->hasValueId());

  const auto &indexedCalls = svfg->getIndCallSites(formalFp->getValueId());
  EXPECT_EQ(indexedCalls.count(indCall), 1u);
}
TEST_F(DDAATest, DDAPassContextModeUsesContextSensitiveAliasing) {
  const char *source = R"(
    define i32* @id(i32* %x) {
      ret i32* %x
    }

    define i32 @main() {
      %a = alloca i32
      %b = alloca i32
      %p = call i32* @id(i32* %a)
      %q = call i32* @id(i32* %b)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  const Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const CallBase *p = nullptr;
  const CallBase *q = nullptr;
  for (const BasicBlock &BB : *mainFn) {
    for (const Instruction &I : BB) {
      const auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      if (!p)
        p = CB;
      else
        q = CB;
    }
  }
  ASSERT_NE(p, nullptr);
  ASSERT_NE(q, nullptr);

  DDAPass flowPass;
  flowPass.setDDAKind(DDAKind::FlowS_DDA);
  flowPass.runOnModule(*module);
  ASSERT_NE(flowPass.getFlowDDA(), nullptr);
  EXPECT_TRUE(flowPass.mayAlias(p, q));

  DDAPass cxtPass;
  cxtPass.setDDAKind(DDAKind::Cxt_DDA);
  cxtPass.runOnModule(*module);
  ASSERT_NE(cxtPass.getContextDDA(), nullptr);
  EXPECT_FALSE(cxtPass.mayAlias(p, q));
}
TEST_F(DDAATest, DDAPassAppliesConfiguredContextLimits) {
  const char *source = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ContextDDA::setMaxCxtLen(3);
  ContextDDA::setMaxPathLen(0);
  const uint32_t oldBudget = FlowDDA::getDefaultMaxBudget();

  DDAPass dda;
  dda.setDDAKind(DDAKind::Cxt_DDA);
  dda.setMaxContextLen(5);
  dda.setMaxPathLen(17);
  dda.setMaxBudget(23);
  dda.runOnModule(*module);

  EXPECT_EQ(ContextCond::getMaxCxtLen(), 5u);
  EXPECT_EQ(ContextCond::getMaxPathLen(), 17u);
  EXPECT_EQ(FlowDDA::getDefaultMaxBudget(), 23u);
  ASSERT_NE(dda.getContextDDA(), nullptr);

  FlowDDA::setDefaultMaxBudget(oldBudget);
}
