TEST_F(PulseCheckerTest, TaintSourceAndSinkProduceReport) {
  auto module = parseModule(context, R"(
    declare i8* @getenv(i8*)
    declare i32 @system(i8*)

    define i32 @taint_flow(i8* %name) {
    entry:
      %v = call i8* @getenv(i8* %name)
      %r = call i32 @system(i8* %v)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(mgr, IssueType::TaintError);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }
  const size_t reportsAfter = getReportCountForType(mgr, IssueType::TaintError);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);
}

TEST_F(PulseCheckerTest, LoopConvergenceRequiresEquivalentPathStamp) {
  auto module = parseModule(context, R"(
    define void @test_loop(i32 %n) {
    entry:
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %loop ]
      %next = add i32 %i, 1
      %cmp = icmp slt i32 %next, %n
      br i1 %cmp, label %loop, label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("test_loop");
  ASSERT_NE(F, nullptr);

  DominatorTree DT(*F);
  LoopInfo LI;
  LI.analyze(DT);

  LoopAbstraction loopAbs;
  loopAbs.initialize(LI);

  BasicBlock *header = nullptr;
  for (auto &BB : *F) {
    if (BB.getName() == "loop") {
      header = &BB;
      break;
    }
  }
  ASSERT_NE(header, nullptr);

  ExecutionDomain firstState;
  ExecutionDomain secondState;
  ExecutionDomain thirdState;

  AbstractValue a(nullptr, 1);
  AbstractValue b(nullptr, 2);
  ASSERT_TRUE(secondState.getAstate()->getPathFormula().addNull(a));
  ASSERT_TRUE(thirdState.getAstate()->getPathFormula().addNull(b));

  EXPECT_FALSE(loopAbs.visitHeader(header, firstState));
  EXPECT_TRUE(loopAbs.visitHeader(header, secondState));
  EXPECT_TRUE(loopAbs.visitHeader(header, thirdState));
  EXPECT_FALSE(loopAbs.hasPreviousIterationSamePathStamp(header));
}

TEST_F(PulseCheckerTest, ReallocAssignmentReportsUseAfterFree) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare i8* @realloc(i8*, i64)

    define void @realloc_alias_uaf() {
    entry:
      %slot = alloca i8*
      %p0 = call i8* @malloc(i64 1)
      store i8* %p0, i8** %slot
      %alias = load i8*, i8** %slot
      %p1 = call i8* @realloc(i8* %p0, i64 2)
      store i8* %p1, i8** %slot
      %v = load i8, i8* %alias
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("realloc_alias_uaf");
  ASSERT_NE(F, nullptr);
  LoadInst *sink = findNthInstruction<LoadInst>(F, 1);
  ASSERT_NE(sink, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, IssueType::UseAfterFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter =
      getReportCountForType(mgr, IssueType::UseAfterFree);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, IssueType::UseAfterFree);
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportContainsTip(report, "Use after free detected"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, sink);
}

TEST_F(PulseCheckerTest, ComputingComparisonDoesNotAssumeItsResult) {
  auto module = parseModule(context, R"(
    define i1 @compare_only(i8* %a, i8* %b) {
    entry:
      %cmp = icmp eq i8* %a, %b
      ret i1 %cmp
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("compare_only");
  ASSERT_NE(F, nullptr);
  auto *cmp = findNthInstruction<ICmpInst>(F, 0);
  ASSERT_NE(cmp, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto states = checker.executeInstruction(cmp, std::move(state), nullptr, 0);
  ASSERT_EQ(states.size(), 1u);
  auto *astate = states.front().getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue a = checker.getFactory().getOrCreate(F->getArg(0));
  AbstractValue b = checker.getFactory().getOrCreate(F->getArg(1));
  EXPECT_FALSE(astate->getPathFormula().areEqual(a, b));
  EXPECT_FALSE(astate->getPathFormula().areDisequal(a, b));
}

TEST_F(PulseCheckerTest, PointerArgumentLoadAbducesPointee) {
  auto module = parseModule(context, R"(
    define i32 @load_arg(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("load_arg");
  ASSERT_NE(F, nullptr);
  auto *load = findNthInstruction<LoadInst>(F, 0);
  ASSERT_NE(load, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto states = checker.executeInstruction(load, std::move(state), nullptr, 0);
  ASSERT_EQ(states.size(), 1u);
  auto *astate = states.front().getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue p = checker.getFactory().getOrCreate(F->getArg(0));
  Access deref(AccessKind::Dereference);
  const Address *postPointee =
      astate->getPostHeap().findEdge(astate->getCanonical(p), deref);
  const Address *prePointee =
      astate->getPreHeap().findEdge(astate->getCanonical(p), deref);
  const Address *loaded = astate->getPostStack().find(load);
  ASSERT_NE(postPointee, nullptr);
  ASSERT_NE(prePointee, nullptr);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(astate->getCanonical(postPointee->addr),
            astate->getCanonical(loaded->addr));
}

TEST_F(PulseCheckerTest, PointerArgumentStoreWritesPointee) {
  auto module = parseModule(context, R"(
    define void @store_arg(i32* %p) {
    entry:
      store i32 7, i32* %p
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("store_arg");
  ASSERT_NE(F, nullptr);
  auto *store = findNthInstruction<StoreInst>(F, 0);
  ASSERT_NE(store, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto states = checker.executeInstruction(store, std::move(state), nullptr, 0);
  ASSERT_EQ(states.size(), 1u);
  auto *astate = states.front().getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue p = checker.getFactory().getOrCreate(F->getArg(0));
  Access deref(AccessKind::Dereference);
  EXPECT_NE(astate->getPostHeap().findEdge(astate->getCanonical(p), deref),
            nullptr);
  EXPECT_EQ(astate->getPreHeap().findEdge(astate->getCanonical(p), deref),
            nullptr);
  EXPECT_TRUE(astate->getPreAttrs().has(astate->getCanonical(p),
                                        pulse::Attribute::Allocated));
  const Address *binding = astate->getPostStack().find(F->getArg(0));
  ASSERT_NE(binding, nullptr);
  EXPECT_EQ(astate->getCanonical(binding->addr), astate->getCanonical(p));
}

TEST_F(PulseCheckerTest, DisjunctReductionDoesNotKeepMovedFromStates) {
  auto module = parseModule(context, R"(
    define void @many_paths() {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  BasicBlock *entry = &module->getFunction("many_paths")->getEntryBlock();

  DisjunctiveDomain domain;
  for (unsigned i = 0; i < 11; ++i) {
    domain.add(entry, ExecutionDomain(), entry);
  }

  const auto &disjuncts = domain.getDisjuncts(entry);
  ASSERT_EQ(disjuncts.size(), 10u);
  for (const auto &disjunct : disjuncts) {
    EXPECT_NE(disjunct.state.getAstate(), nullptr);
  }
}

TEST_F(PulseCheckerTest, SummaryStoreInitializesCallerPointee) {
  auto module = parseModule(context, R"(
    define void @init(i32* %p) {
    entry:
      store i32 1, i32* %p
      ret void
    }

    define void @caller() {
    entry:
      %x = alloca i32
      call void @init(i32* %x)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *init = module->getFunction("init");
  Function *caller = module->getFunction("caller");
  ASSERT_NE(init, nullptr);
  ASSERT_NE(caller, nullptr);
  auto *allocaInst = findNthInstruction<AllocaInst>(caller, 0);
  auto *call = dyn_cast<CallInst>(findCallTo(caller, "init"));
  ASSERT_NE(allocaInst, nullptr);
  ASSERT_NE(call, nullptr);

  PulseChecker checker(module.get());
  checker.analyzeFunction(init);
  ExecutionDomain state = checker.initializeFunction(caller);
  auto allocaStates =
      checker.executeInstruction(allocaInst, std::move(state), nullptr, 0);
  ASSERT_EQ(allocaStates.size(), 1u);
  auto callStates = checker.executeInstruction(
      call, std::move(allocaStates.front()), nullptr, 0);
  ASSERT_FALSE(callStates.empty());
  auto *astate = callStates.front().getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue x = checker.getFactory().getOrCreate(allocaInst);
  EXPECT_FALSE(astate->getPostAttrs().has(astate->getCanonical(x),
                                          pulse::Attribute::Uninitialized));
  EXPECT_NE(astate->getPostHeap().findEdge(astate->getCanonical(x),
                                           Access(AccessKind::Dereference)),
            nullptr);
}

TEST_F(PulseCheckerTest, SummaryPreservesNestedMaterializedObjectMapping) {
  auto module = parseModule(context, R"(
    define void @free_nested(i8*** %root) {
    entry:
      ret void
    }

    define void @caller(i8*** %root) {
    entry:
      call void @free_nested(i8*** %root)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *freeNext = module->getFunction("free_nested");
  Function *caller = module->getFunction("caller");
  ASSERT_NE(freeNext, nullptr);
  ASSERT_NE(caller, nullptr);
  auto *freeNextCall = dyn_cast<CallInst>(findCallTo(caller, "free_nested"));
  ASSERT_NE(freeNextCall, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(caller);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue root = checker.getFactory().getOrCreate(caller->getArg(0));
  AbstractValue child = checker.getFactory().createFresh();
  AbstractValue next = checker.getFactory().createFresh();
  astate->getPostHeap().addEdge(root, Access(AccessKind::Dereference),
                                Address(child));
  astate->getPostHeap().addEdge(child, Access(AccessKind::Dereference),
                                Address(next));
  checker.getOperations().allocate(*astate, root, nullptr);
  checker.getOperations().allocate(*astate, child, nullptr);
  checker.getOperations().allocate(*astate, next, nullptr);

  AbstractValue formalRoot =
      checker.getFactory().getOrCreate(freeNext->getArg(0));
  AbstractValue formalChild = checker.getFactory().createFresh();
  AbstractValue formalNext = checker.getFactory().createFresh();

  auto summaryPre = std::make_unique<AbductiveDomain>();
  summaryPre->getPreStack().add(freeNext->getArg(0), Address(formalRoot));
  summaryPre->getPreHeap().addEdge(formalRoot,
                                   Access(AccessKind::Dereference),
                                   Address(formalChild));
  summaryPre->getPreHeap().addEdge(formalChild,
                                   Access(AccessKind::Dereference),
                                   Address(formalNext));
  summaryPre->getPreAttrs().add(formalRoot, pulse::Attribute::Allocated);
  summaryPre->getPreAttrs().add(formalChild, pulse::Attribute::Allocated);

  auto summaryPost = std::make_unique<AbductiveDomain>();
  summaryPost->getPostStack().add(freeNext->getArg(0), Address(formalRoot));
  summaryPost->getPostHeap().addEdge(formalRoot,
                                     Access(AccessKind::Dereference),
                                     Address(formalChild));
  summaryPost->getPostHeap().addEdge(formalChild,
                                     Access(AccessKind::Dereference),
                                     Address(formalNext));
  summaryPost->getPostAttrs().add(formalNext, pulse::Attribute::Invalid);

  PulseSummary summary(freeNext);
  summary.setFormalAV(freeNext->getArg(0), formalRoot);
  summary.addPrePost(SummaryEntry(
      std::move(summaryPre), PulseFormula(), std::move(summaryPost),
      PulseFormula(), std::nullopt));

  auto states = checker.applySummaryImproved(
      freeNext, state, freeNextCall, nullptr, &summary);
  ASSERT_FALSE(states.empty());
  astate = states.front().getAstate();
  ASSERT_NE(astate, nullptr);
  EXPECT_TRUE(astate->getPostAttrs().has(astate->getCanonical(next),
                                         pulse::Attribute::Invalid));
}
