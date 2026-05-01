#include "SaberConditionAllocatorTestSupport.h"

TEST(SaberConditionAllocatorTest, MultiSuccessorGuardsAreExhaustiveAndExclusive) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @f(i32 %x) {
    entry:
      switch i32 %x, label %default [
        i32 0, label %case0
        i32 1, label %case1
        i32 2, label %case2
      ]
    case0:
      ret i32 0
    case1:
      ret i32 1
    case2:
      ret i32 2
    default:
      ret i32 3
    }
  )");
  ASSERT_NE(module, nullptr);

  const BasicBlock *entry = getBlock(*module, "f", "entry");
  ASSERT_NE(entry, nullptr);

  SaberCondAllocator allocator;
  allocator.setModule(module.get());
  allocator.allocate();

  std::vector<SaberCondAllocator::Condition> guards;
  auto disjunction = allocator.getFalseCond();
  for (const BasicBlock *succ : successors(entry)) {
    auto guard = allocator.getBranchCond(entry, succ);
    guards.push_back(guard);
    disjunction = allocator.condOr(disjunction, guard);
  }

  ASSERT_EQ(guards.size(), 4u);
  EXPECT_TRUE(
      allocator.isEquivalentBranchCond(disjunction, allocator.getTrueCond()));
  for (size_t i = 0; i < guards.size(); ++i) {
    for (size_t j = i + 1; j < guards.size(); ++j) {
      EXPECT_TRUE(allocator.isEquivalentBranchCond(
          allocator.condAnd(guards[i], guards[j]), allocator.getFalseCond()));
    }
  }
}
TEST(SaberConditionAllocatorTest, FourWayBranchUsesCeilLog2DecisionVariables) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    define i32 @f(i32 %x) {
    entry:
      switch i32 %x, label %default [
        i32 0, label %case0
        i32 1, label %case1
        i32 2, label %case2
      ]
    case0:
      ret i32 0
    case1:
      ret i32 1
    case2:
      ret i32 2
    default:
      ret i32 3
    }
  )");
  ASSERT_NE(module, nullptr);

  SaberCondAllocator allocator;
  allocator.setModule(module.get());
  allocator.allocate();

  EXPECT_EQ(allocator.getCondNum(), 2u);
}
TEST(SaberConditionAllocatorTest, ResetDropsConditionStateBetweenModules) {
  LLVMContext context;
  auto firstModule = parseModule(context, R"(
    define void @first(i1 %cond) {
    entry:
      br i1 %cond, label %lhs, label %rhs
    lhs:
      ret void
    rhs:
      ret void
    }
  )");
  auto secondModule = parseModule(context, R"(
    define void @second(i32 %x) {
    entry:
      switch i32 %x, label %default [
        i32 0, label %case0
        i32 1, label %case1
      ]
    case0:
      ret void
    case1:
      ret void
    default:
      ret void
    }
  )");
  ASSERT_NE(firstModule, nullptr);
  ASSERT_NE(secondModule, nullptr);

  SaberCondAllocator allocator;
  allocator.setModule(firstModule.get());
  allocator.allocate();
  EXPECT_EQ(allocator.getCondNum(), 1u);

  allocator.reset();
  allocator.setModule(secondModule.get());
  allocator.allocate();
  EXPECT_EQ(allocator.getCondNum(), 2u);
}
TEST(SaberConditionAllocatorTest,
     LoopWithOnlyProgramExitEdgesUseTrueExitGuardLikeSVF) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare void @abort()

    define void @f(i1 %cond) {
    entry:
      br label %loop
    loop:
      br i1 %cond, label %abortbb, label %loop
    abortbb:
      call void @abort()
      unreachable
    }
  )");
  ASSERT_NE(module, nullptr);

  const Function *function = module->getFunction("f");
  ASSERT_NE(function, nullptr);
  const BasicBlock *loop = getBlock(*module, "f", "loop");
  const BasicBlock *abortbb = getBlock(*module, "f", "abortbb");
  ASSERT_NE(loop, nullptr);
  ASSERT_NE(abortbb, nullptr);

  SaberCondAllocator allocator;
  allocator.setModule(module.get());
  allocator.initDominatorsForFunction(function);
  allocator.initPostDominatorsForFunction(function);
  allocator.initLoopInfoForFunction(function);
  allocator.allocate();

  auto guard = allocator.evaluateLoopExitBranch(loop, abortbb);
  EXPECT_EQ(allocator.dumpCond(guard),
            allocator.dumpCond(allocator.getTrueCond()));
}
TEST(SaberConditionAllocatorTest, SetModulePreservesImportedSourceSinkState) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    define void @f() {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  DummySrcSnkDDA checker;
  char src_storage = 0;
  char sink_storage = 0;
  char call_storage = 0;
  SrcSnkDDA::SVFGNodeSet sources = {
      reinterpret_cast<const SVFGNode *>(&src_storage)};
  SrcSnkDDA::SVFGNodeSet sinks = {
      reinterpret_cast<const SVFGNode *>(&sink_storage)};
  SrcSnkDDA::SrcToCSMap srcToCS = {
      {reinterpret_cast<const SVFGNode *>(&src_storage),
       reinterpret_cast<const CallBase *>(&call_storage)}};

  checker.importSourceSinkState(sources, sinks, srcToCS);
  checker.setModule(module.get());

  SrcSnkDDA::SVFGNodeSet exportedSources;
  SrcSnkDDA::SVFGNodeSet exportedSinks;
  SrcSnkDDA::SrcToCSMap exportedSrcToCS;
  checker.exportSourceSinkState(exportedSources, exportedSinks, exportedSrcToCS);

  EXPECT_EQ(exportedSources, sources);
  EXPECT_EQ(exportedSinks, sinks);
  EXPECT_EQ(exportedSrcToCS, srcToCS);
}
TEST(SaberConditionAllocatorTest,
     SharedGraphInitializationPreservesRemovedStrongUpdateEdges) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    define void @f() {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  DummySrcSnkDDA checker;
  checker.setSharedSVFGAndICFG(std::make_unique<SVFG>(),
                               std::make_unique<ICFG>());
  checker.setModule(module.get());

  char src_storage = 0;
  char sink_storage = 0;
  auto *src = reinterpret_cast<const SVFGNode *>(&src_storage);
  auto *sink = reinterpret_cast<const SVFGNode *>(&sink_storage);
  checker.getSaberCondAllocator()->getRemovedSUVFEdges()[src].insert(sink);

  checker.initialize();

  const auto &removed = checker.getSaberCondAllocator()->getRemovedSUVFEdges();
  ASSERT_EQ(removed.size(), 1u);
  auto it = removed.find(src);
  ASSERT_NE(it, removed.end());
  EXPECT_EQ(it->second.size(), 1u);
  EXPECT_EQ(*it->second.begin(), sink);
  EXPECT_TRUE(checker.hasSVFGAndICFG());
}
TEST(SaberConditionAllocatorTest,
     SharedGraphTransferPreservesRemovedStrongUpdateEdgesAcrossCheckers) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    define void @f() {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  DummySrcSnkDDA producer;
  producer.setSharedSVFGAndICFG(std::make_unique<SVFG>(),
                                std::make_unique<ICFG>());
  producer.setModule(module.get());

  char src_storage = 0;
  char sink_storage = 0;
  auto *src = reinterpret_cast<const SVFGNode *>(&src_storage);
  auto *sink = reinterpret_cast<const SVFGNode *>(&sink_storage);
  producer.getSaberCondAllocator()->getRemovedSUVFEdges()[src].insert(sink);

  SrcSnkDDA::RemovedSUVFEdges exportedRemovedEdges;
  producer.exportRemovedSUVFEdges(exportedRemovedEdges);
  auto extracted = producer.extractSVFGAndICFG();

  DummySrcSnkDDA consumer;
  consumer.setSharedSVFGAndICFG(std::move(extracted.first),
                                std::move(extracted.second));
  consumer.importRemovedSUVFEdges(exportedRemovedEdges);
  consumer.setModule(module.get());
  consumer.initialize();

  const auto &removed = consumer.getSaberCondAllocator()->getRemovedSUVFEdges();
  ASSERT_EQ(removed.size(), 1u);
  auto it = removed.find(src);
  ASSERT_NE(it, removed.end());
  EXPECT_EQ(it->second.size(), 1u);
  EXPECT_EQ(*it->second.begin(), sink);
  EXPECT_TRUE(consumer.hasSVFGAndICFG());
}
TEST(SaberConditionAllocatorTest,
     ChangingModuleInvalidatesSharedGraphsAndImportedState) {
  LLVMContext context;
  auto firstModule = parseModule(context, R"(
    define void @f() {
    entry:
      ret void
    }
  )");
  auto secondModule = parseModule(context, R"(
    define void @g() {
    entry:
      ret void
    }
  )");
  ASSERT_NE(firstModule, nullptr);
  ASSERT_NE(secondModule, nullptr);

  DummySrcSnkDDA checker;
  checker.setSharedSVFGAndICFG(std::make_unique<SVFG>(),
                               std::make_unique<ICFG>());

  char src_storage = 0;
  char sink_storage = 0;
  char call_storage = 0;
  auto *src = reinterpret_cast<const SVFGNode *>(&src_storage);
  auto *sink = reinterpret_cast<const SVFGNode *>(&sink_storage);
  checker.importSourceSinkState(
      {src}, {sink},
      {{src, reinterpret_cast<const CallBase *>(&call_storage)}});
  checker.getSaberCondAllocator()->getRemovedSUVFEdges()[src].insert(sink);

  checker.setModule(firstModule.get());
  EXPECT_TRUE(checker.hasSVFGAndICFG());
  EXPECT_FALSE(checker.getSources().empty());
  EXPECT_FALSE(checker.getSinks().empty());
  EXPECT_FALSE(checker.getSaberCondAllocator()->getRemovedSUVFEdges().empty());

  checker.setModule(secondModule.get());
  EXPECT_FALSE(checker.hasSVFGAndICFG());
  EXPECT_TRUE(checker.getSources().empty());
  EXPECT_TRUE(checker.getSinks().empty());
  EXPECT_TRUE(
      checker.getSaberCondAllocator()->getRemovedSUVFEdges().empty());
}
TEST(SaberConditionAllocatorTest,
     InitializeResolvesIndirectCallsForSourceSinkTraversal) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    define i8* @target() {
    entry:
      ret i8* null
    }

    define i8* @caller(i8* ()* %fp) {
    entry:
      %r = call i8* %fp()
      ret i8* %r
    }

    define i32 @main() {
    entry:
      %r = call i8* @caller(i8* ()* @target)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  DummySrcSnkDDA checker;
  checker.setModule(module.get());
  checker.initialize();

  const CallBase *indirectCall = getOnlyCall(*module, "caller");
  const Function *target = module->getFunction("target");
  ASSERT_NE(indirectCall, nullptr);
  ASSERT_NE(target, nullptr);
  ASSERT_NE(checker.getSVFG(), nullptr);

  EXPECT_NE(checker.getSVFG()->getCallSiteId(indirectCall, target), 0u);
}
TEST(SaberConditionAllocatorTest,
     SharedGraphInitializationPreservesIndirectSourceSinkResolution) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define i8* @alloc_wrapper(i8* (i64)* %fp) {
    entry:
      %p = call i8* %fp(i64 4)
      ret i8* %p
    }

    define i32 @main() {
    entry:
      %p = call i8* @alloc_wrapper(i8* (i64)* @malloc)
      %isnull = icmp eq i8* %p, null
      %ret = zext i1 %isnull to i32
      ret i32 %ret
    }
  )");
  ASSERT_NE(module, nullptr);

  LeakChecker producer;
  producer.setModule(module.get());
  producer.initialize();
  ASSERT_EQ(producer.getSources().size(), 1u);

  auto extracted = producer.extractSVFGAndICFG();

  LeakChecker consumer;
  consumer.setSharedSVFGAndICFG(std::move(extracted.first),
                                std::move(extracted.second));
  consumer.setModule(module.get());
  consumer.initialize();

  EXPECT_EQ(consumer.getSources().size(), 1u);
}
TEST(SaberConditionAllocatorTest,
     LeakCheckerAddsExtraLoadSinkOnlyForMultiLevelFreeApis) {
  LLVMContext context;
  auto xfreeModule = parseModule(context, R"(
    declare void @XFree(i8**)

    define void @test() {
    entry:
      %slot = alloca i8*
      store i8* null, i8** %slot
      %loaded = load i8*, i8** %slot
      call void @XFree(i8** %slot)
      ret void
    }

    define i32 @main() {
    entry:
      call void @test()
      ret i32 0
    }
  )");
  auto freeModule = parseModule(context, R"(
    declare void @free(i8**)

    define void @test() {
    entry:
      %slot = alloca i8*
      store i8* null, i8** %slot
      %loaded = load i8*, i8** %slot
      call void @free(i8** %slot)
      ret void
    }

    define i32 @main() {
    entry:
      call void @test()
      ret i32 0
    }
  )");
  ASSERT_NE(xfreeModule, nullptr);
  ASSERT_NE(freeModule, nullptr);

  LeakChecker xfreeChecker;
  xfreeChecker.setModule(xfreeModule.get());
  xfreeChecker.initialize();
  EXPECT_EQ(xfreeChecker.getSinks().size(), 2u);

  LeakChecker freeChecker;
  freeChecker.setModule(freeModule.get());
  freeChecker.initialize();
  EXPECT_EQ(freeChecker.getSinks().size(), 1u);
}
TEST(SaberConditionAllocatorTest,
     LeakCheckerAddsExtraLoadSinkThroughBitcastForMultiLevelFreeApis) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare void @XFree(i8**)

    define void @test() {
    entry:
      %slot = alloca i8*
      store i8* null, i8** %slot
      %slot.cast = bitcast i8** %slot to i8**
      %loaded = load i8*, i8** %slot.cast
      call void @XFree(i8** %slot)
      ret void
    }

    define i32 @main() {
    entry:
      call void @test()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  LeakChecker checker;
  checker.setModule(module.get());
  checker.initialize();

  EXPECT_EQ(checker.getSinks().size(), 2u);
}
TEST(SaberConditionAllocatorTest,
     LeakCheckerDoesNotAddExtraLoadSinkThroughPhiForMultiLevelFreeApis) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare void @XFree(i8**)

    define void @test(i1 %cond) {
    entry:
      %slot = alloca i8*
      store i8* null, i8** %slot
      br i1 %cond, label %left, label %right

    left:
      br label %merge

    right:
      br label %merge

    merge:
      %slot.phi = phi i8** [ %slot, %left ], [ %slot, %right ]
      %loaded = load i8*, i8** %slot.phi
      call void @XFree(i8** %slot)
      ret void
    }

    define i32 @main() {
    entry:
      call void @test(i1 true)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  LeakChecker checker;
  checker.setModule(module.get());
  checker.initialize();

  EXPECT_EQ(checker.getSinks().size(), 1u);
}
TEST(SaberConditionAllocatorTest,
     StrongUpdateIsDisabledForIndirectRecursiveStackObjects) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    @fp = global void ()* @f

    define void @f() {
    entry:
      %slot = alloca i8*
      store i8* null, i8** %slot
      %callee = load void ()*, void ()** @fp
      call void %callee()
      ret void
    }

    define i32 @main() {
    entry:
      call void @f()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  auto icfg = std::make_unique<ICFG>();
  ICFGBuilder icfgBuilder(icfg.get());
  icfgBuilder.build(module.get());

  TestSaberSVFGBuilder builder;
  builder.setModule(module.get());
  SVFG *svfg = builder.buildSVFG(icfg.get());
  ASSERT_NE(svfg, nullptr);

  const StoreInst *store = getOnlyStore(*module, "f");
  ASSERT_NE(store, nullptr);
  SVFGNode *storeNode = svfg->getDef(store);
  ASSERT_NE(storeNode, nullptr);

  uint32_t singleton = 0;
  EXPECT_FALSE(builder.isStrongUpdatePublic(storeNode, singleton));
}
TEST(SaberConditionAllocatorTest,
     StrongUpdateIsDisabledForUnknownObjectStores) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8** @unknown_pp()

    define void @test(i8* %value) {
    entry:
      %pp = call i8** @unknown_pp()
      store i8* %value, i8** %pp
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto icfg = std::make_unique<ICFG>();
  ICFGBuilder icfgBuilder(icfg.get());
  icfgBuilder.build(module.get());

  TestSaberSVFGBuilder builder;
  builder.setModule(module.get());

  SVFGBuilderConfig cfg;
  cfg.usePointerAnalysis = false;
  cfg.buildMSSA = true;
  SVFG *svfg = builder.build(icfg.get(), cfg);
  ASSERT_NE(svfg, nullptr);
  builder.setCurrentSVFG(svfg);

  const StoreInst *store = getOnlyStore(*module, "test");
  ASSERT_NE(store, nullptr);
  SVFGNode *storeNode = svfg->getDef(store);
  ASSERT_NE(storeNode, nullptr);

  const Value *storePtr = store->getPointerOperand();
  ASSERT_NE(storePtr, nullptr);
  const uint32_t unknownObjId = builder.getUnknownObjId();
  ASSERT_NE(unknownObjId, 0u);
  svfg->setObjectValue(unknownObjId, storePtr);
  ASSERT_EQ(svfg->getObjectId(storePtr), unknownObjId);
  ASSERT_TRUE(svfg->isUnknownObject(unknownObjId));

  uint32_t singleton = 0;
  EXPECT_FALSE(builder.isStrongUpdatePublic(storeNode, singleton));
}
TEST(SaberConditionAllocatorTest, LeakCheckerSkipsUncalledFunctionsLikeSVF) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @cleanup() {
    entry:
      call void @free(i8* null)
      ret void
    }

    define void @helper() {
    entry:
      %p = call i8* @malloc(i64 4)
      ret void
    }

    define i32 @main() {
    entry:
      call void @cleanup()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Memory Leak");

  LeakChecker checker;
  checker.runOnModule(*module);

  EXPECT_TRUE(checker.getSources().empty());
  EXPECT_EQ(getReportCountForType(mgr, "Memory Leak"), reportsBefore);
}
TEST(SaberConditionAllocatorTest,
     LeakCheckerReportsDeadCallChainSourcesLikeSVF) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @helper() {
    entry:
      %p = call i8* @malloc(i64 4)
      ret void
    }

    define void @dead() {
    entry:
      call void @helper()
      ret void
    }

    define void @cleanup() {
    entry:
      call void @free(i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      call void @cleanup()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Memory Leak");

  LeakChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Memory Leak");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, "Memory Leak");
  ASSERT_NE(report, nullptr);
  ASSERT_FALSE(report->get_steps().empty());
  const auto *call = dyn_cast_or_null<CallBase>(report->get_steps().front()->inst);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(call->getCalledFunction(), nullptr);
  EXPECT_EQ(call->getCalledFunction()->getName(), "malloc");
}
TEST(SaberConditionAllocatorTest, LeakCheckerReportsCallsiteAsSourceStep) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @cleanup() {
    entry:
      call void @free(i8* null)
      ret void
    }

    define void @leak() {
    entry:
      %p = call i8* @malloc(i64 4)
      ret void
    }

    define i32 @main() {
    entry:
      call void @leak()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Memory Leak");

  LeakChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Memory Leak");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, "Memory Leak");
  ASSERT_NE(report, nullptr);
  ASSERT_FALSE(report->get_steps().empty());

  const BugDiagStep *sourceStep = report->get_steps().front();
  ASSERT_NE(sourceStep, nullptr);
  EXPECT_EQ(sourceStep->tip, "Memory allocated here is never freed");
  const auto *call = dyn_cast_or_null<CallBase>(sourceStep->inst);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(call->getCalledFunction(), nullptr);
  EXPECT_EQ(call->getCalledFunction()->getName(), "malloc");
  EXPECT_FALSE(reportHasStepTip(report, "Memory deallocated here"));
}
TEST(SaberConditionAllocatorTest,
     LeakCheckerReportsNeverFreeWhenNoSinkExistsAnywhere) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define i32 @main() {
    entry:
      %p = call i8* @malloc(i64 4)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Memory Leak");

  LeakChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Memory Leak");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, "Memory Leak");
  ASSERT_NE(report, nullptr);
  ASSERT_FALSE(report->get_steps().empty());
  EXPECT_EQ(report->get_steps().front()->tip,
            "Memory allocated here is never freed");
}
TEST(SaberConditionAllocatorTest,
     DoubleFreeCheckerReportsAllocationCallsiteAsSourceStep) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define void @df() {
    entry:
      %p = call i8* @malloc(i64 4)
      call void @free(i8* %p)
      call void @free(i8* %p)
      ret void
    }

    define i32 @main() {
    entry:
      call void @df()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Double Free");

  DoubleFreeChecker checker;
  checker.runOnModule(*module);

  const size_t reportsAfter = getReportCountForType(mgr, "Double Free");
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, "Double Free");
  ASSERT_NE(report, nullptr);
  ASSERT_FALSE(report->get_steps().empty());

  const BugDiagStep *sourceStep = report->get_steps().front();
  ASSERT_NE(sourceStep, nullptr);
  EXPECT_EQ(sourceStep->tip, "Memory allocated here");
  const auto *call = dyn_cast_or_null<CallBase>(sourceStep->inst);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(call->getCalledFunction(), nullptr);
  EXPECT_EQ(call->getCalledFunction()->getName(), "malloc");
  EXPECT_FALSE(
      reportHasStepTip(report, "Memory deallocated along double-free path"));
}
TEST(SaberConditionAllocatorTest,
     DefaultModeHandlesStackMediatedMallocFreeWithoutFalseLeak) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i32 @main() {
    entry:
      %slot = alloca i8*
      %p = call i8* @malloc(i64 4)
      store i8* %p, i8** %slot
      %q = load i8*, i8** %slot
      call void @free(i8* %q)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, "Memory Leak");

  LeakChecker checker;
  checker.runOnModule(*module);

  EXPECT_EQ(getReportCountForType(mgr, "Memory Leak"), reportsBefore);
}
TEST(SaberConditionAllocatorTest,
     LeakCheckerTreatsAssertFailBranchAsProgramExitLikeSVF) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare noalias i8* @malloc(i64)
    declare void @free(i8*)
    declare void @__assert_fail(i8*, i8*, i32, i8*) noreturn

    @.str = private unnamed_addr constant [5 x i8] c"cond\00"
    @.file = private unnamed_addr constant [5 x i8] c"test\00"
    @.func = private unnamed_addr constant [5 x i8] c"test\00"

    define void @test(i1 %ok) {
    entry:
      %p = call i8* @malloc(i64 4)
      br i1 %ok, label %freebb, label %assertbb

    freebb:
      call void @free(i8* %p)
      ret void

    assertbb:
      %msg = getelementptr inbounds [5 x i8], [5 x i8]* @.str, i64 0, i64 0
      %file = getelementptr inbounds [5 x i8], [5 x i8]* @.file, i64 0, i64 0
      %func = getelementptr inbounds [5 x i8], [5 x i8]* @.func, i64 0, i64 0
      call void @__assert_fail(i8* %msg, i8* %file, i32 1, i8* %func)
      unreachable
    }

    define i32 @main() {
    entry:
      call void @test(i1 false)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getMemoryLeakReportCount(mgr);

  LeakChecker checker;
  checker.runOnModule(*module);

  EXPECT_EQ(getMemoryLeakReportCount(mgr), reportsBefore);
}
TEST(SaberConditionAllocatorTest,
     LeakCheckerDoesNotTreatAbortBranchAsProgramExitLikeSVF) {
  LLVMContext context;
  auto module = parseModule(context, R"(
    declare noalias i8* @malloc(i64)
    declare void @free(i8*)
    declare void @abort() noreturn

    define void @test(i1 %ok) {
    entry:
      %p = call i8* @malloc(i64 4)
      br i1 %ok, label %freebb, label %abortbb

    freebb:
      call void @free(i8* %p)
      ret void

    abortbb:
      call void @abort()
      unreachable
    }

    define i32 @main() {
    entry:
      call void @test(i1 false)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getMemoryLeakReportCount(mgr);

  LeakChecker checker;
  checker.runOnModule(*module);

  EXPECT_EQ(getMemoryLeakReportCount(mgr), reportsBefore + 1);
}
