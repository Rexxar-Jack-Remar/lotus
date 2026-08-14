TEST_F(PulseCheckerTest, ReportsNullDereferenceWithConcreteWitness) {
  auto module = parseModule(context, R"(
    define i8 @null_deref() {
    entry:
      %v = load i8, i8* null
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("null_deref");
  ASSERT_NE(F, nullptr);
  LoadInst *sink = findNthInstruction<LoadInst>(F, 0);
  ASSERT_NE(sink, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, IssueType::NullDereference);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter =
      getReportCountForType(mgr, IssueType::NullDereference);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report =
      getLastReportForType(mgr, IssueType::NullDereference);
  ASSERT_NE(report, nullptr);
  ASSERT_TRUE(reportContainsTip(report, "Null pointer dereference"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, sink);
}

TEST_F(PulseCheckerTest, ConstantFalseBranchDoesNotReportUnreachableFault) {
  auto module = parseModule(context, R"(
    define i32 @unreachable_null_dereference() {
    entry:
      br i1 false, label %bad, label %ok

    bad:
      %x = load i32, i32* null
      ret i32 %x

    ok:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before =
      getReportCountForType(mgr, IssueType::NullDereference);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), before);
}

TEST_F(PulseCheckerTest, LoadedFalseBranchDoesNotReportUnreachableFault) {
  auto module = parseModule(context, R"(
    define i32 @loaded_false_branch() {
    entry:
      %condition = alloca i1
      store i1 false, i1* %condition
      %loaded = load i1, i1* %condition
      br i1 %loaded, label %bad, label %ok

    bad:
      %x = load i32, i32* null
      ret i32 %x

    ok:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before =
      getReportCountForType(mgr, IssueType::NullDereference);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), before);
}

TEST_F(PulseCheckerTest, ConstantFalsePointerSelectUsesValidOperandOnly) {
  auto module = parseModule(context, R"(
    define i8 @constant_false_select() {
    entry:
      %valid = alloca i8
      store i8 7, i8* %valid
      %selected = select i1 false, i8* null, i8* %valid
      %value = load i8, i8* %selected
      ret i8 %value
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before =
      getReportCountForType(mgr, IssueType::NullDereference);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::NullDereference), before);
}

TEST_F(PulseCheckerTest, ReportsUninitializedReadAtLoadSite) {
  auto module = parseModule(context, R"(
    define i32 @uninit_read() {
    entry:
      %x = alloca i32
      %v = load i32, i32* %x
      ret i32 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("uninit_read");
  ASSERT_NE(F, nullptr);
  LoadInst *load = findNthInstruction<LoadInst>(F, 0);
  ASSERT_NE(load, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, IssueType::UninitializedRead);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter =
      getReportCountForType(mgr, IssueType::UninitializedRead);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report =
      getLastReportForType(mgr, IssueType::UninitializedRead);
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportContainsTip(report, "Load from uninitialized variable"));
  EXPECT_TRUE(reportContainsTip(report, "Reading uninitialized memory"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, load);
}

TEST_F(PulseCheckerTest, ReportsInvalidFreeOfStackPointer) {
  auto module = parseModule(context, R"(
    declare void @free(i8*)

    define void @bad_free() {
    entry:
      %stack = alloca i8
      call void @free(i8* %stack)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("bad_free");
  ASSERT_NE(F, nullptr);
  auto *freeCall = dyn_cast<CallInst>(findCallTo(F, "free"));
  ASSERT_NE(freeCall, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore =
      getReportCountForType(mgr, IssueType::InvalidFree);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter =
      getReportCountForType(mgr, IssueType::InvalidFree);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report = getLastReportForType(mgr, IssueType::InvalidFree);
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportContainsTip(report, "Invalid free: non-heap pointer"));
  EXPECT_TRUE(reportContainsTip(report, "Invalid free of stack memory"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, freeCall);
}

TEST_F(PulseCheckerTest, ReportsStackAddressEscapeViaReturn) {
  auto module = parseModule(context, R"(
    define i8* @return_local() {
    entry:
      %x = alloca i8
      ret i8* %x
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("return_local");
  ASSERT_NE(F, nullptr);
  auto *ret = dyn_cast<ReturnInst>(F->getEntryBlock().getTerminator());
  ASSERT_NE(ret, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t reportsBefore = getReportCountForType(
      mgr, IssueType::StackVariableAddressEscape);
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  const size_t reportsAfter = getReportCountForType(
      mgr, IssueType::StackVariableAddressEscape);
  ASSERT_EQ(reportsAfter, reportsBefore + 1);

  const BugReport *report =
      getLastReportForType(mgr, IssueType::StackVariableAddressEscape);
  ASSERT_NE(report, nullptr);
  EXPECT_TRUE(reportContainsTip(report,
                                "Returning address derived from stack "
                                "allocation"));
  EXPECT_TRUE(reportContainsTip(report, "Stack address escapes via return"));

  const BugDiagStep *bugStep = getLastStep(report);
  ASSERT_NE(bugStep, nullptr);
  EXPECT_EQ(bugStep->inst, ret);
}

TEST_F(PulseCheckerTest, LatentIssuesAreNotReportedAtShutdown) {
  auto module = parseModule(context, R"(
    define void @latent_branch() {
    entry:
      %p = alloca i8
      %cmp = icmp eq i8* %p, null
      br i1 %cmp, label %bad, label %ok

    bad:
      ret void

    ok:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const int reportsBefore = mgr.get_total_reports();
  {
    PulseChecker checker(module.get());
    checker.analyze();
  }

  EXPECT_EQ(mgr.get_total_reports(), reportsBefore);
}

TEST_F(PulseCheckerTest, UnlockDoesNotInvalidateLockMemory) {
  auto module = parseModule(context, R"(
    declare i32 @pthread_mutex_unlock(i8*)

    define void @unlock_ok() {
    entry:
      %m = alloca i8
      %r = call i32 @pthread_mutex_unlock(i8* %m)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("unlock_ok");
  ASSERT_NE(F, nullptr);

  auto *allocaInst = dyn_cast<AllocaInst>(&*F->getEntryBlock().begin());
  ASSERT_NE(allocaInst, nullptr);
  auto *unlockCall = dyn_cast<CallInst>(&*std::next(F->getEntryBlock().begin()));
  ASSERT_NE(unlockCall, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = executeEntryBlock(checker, F, unlockCall);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue lockAddr = checker.getFactory().getOrCreate(allocaInst);
  EXPECT_FALSE(astate->getPostAttrs().has(lockAddr, pulse::Attribute::Invalid));
}

TEST_F(PulseCheckerTest, NonReallocAssignmentDoesNotInvalidatePreviousValue) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define void @assign(i8** %slot) {
    entry:
      %new = call i8* @malloc(i64 8)
      store i8* %new, i8** %slot
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("assign");
  ASSERT_NE(F, nullptr);
  Argument *slotArg = F->arg_empty() ? nullptr : &*F->arg_begin();
  ASSERT_NE(slotArg, nullptr);

  auto callIt = F->getEntryBlock().begin();
  auto *mallocCall = dyn_cast<CallInst>(&*callIt);
  ASSERT_NE(mallocCall, nullptr);
  auto *store = dyn_cast<StoreInst>(&*std::next(callIt));
  ASSERT_NE(store, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  AbstractValue oldValue = checker.getFactory().createFresh();
  checker.getOperations().allocate(*astate, oldValue, nullptr);
  astate->getPostStack().add(slotArg, Address(oldValue));
  astate->getPostAttrs().remove(oldValue, pulse::Attribute::Invalid);
  astate->getPostAttrs().remove(oldValue, pulse::Attribute::Uninitialized);

  auto mallocStates =
      checker.executeInstruction(mallocCall, std::move(state), nullptr, 0);
  ASSERT_EQ(mallocStates.size(), 1u);
  state = std::move(mallocStates.front());

  auto storeStates =
      checker.executeInstruction(store, std::move(state), nullptr, 0);
  ASSERT_EQ(storeStates.size(), 1u);
  astate = storeStates.front().getAstate();
  ASSERT_NE(astate, nullptr);

  EXPECT_FALSE(
      astate->getPostAttrs().has(oldValue, pulse::Attribute::Invalid));
}
