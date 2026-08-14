TEST_F(PulseCheckerTest, NullFormalBranchDereferenceIsReported) {
  auto module = parseModule(context, R"(
    define i8 @null_formal(i8* %p) {
    entry:
      %isnull = icmp eq i8* %p, null
      br i1 %isnull, label %bad, label %ok

    bad:
      %v = load i8, i8* %p
      ret i8 %v

    ok:
      ret i8 0
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("null_formal");
  ASSERT_NE(F, nullptr);
  auto *load = findNthInstruction<LoadInst>(F, 0);
  ASSERT_NE(load, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);
  AbstractValue p = checker.getFactory().getOrCreate(F->getArg(0));
  ASSERT_TRUE(astate->getPathFormula().addNull(p));

  auto states = checker.executeInstruction(load, std::move(state), nullptr, 0);
  ASSERT_EQ(states.size(), 1u);
  ASSERT_TRUE(states.front().isStopped());
  const auto &stopped = states.front().getStoppedExecution();
  EXPECT_EQ(stopped.diagnostic, OperationResult::NullDereference);
}

TEST_F(PulseCheckerTest, SwitchDropsInfeasibleCaseWitness) {
  auto module = parseModule(context, R"(
    define void @infeasible_switch(i32 %x) {
    entry:
      %not_one = icmp ne i32 %x, 1
      br i1 %not_one, label %dispatch, label %exit

    dispatch:
      switch i32 %x, label %exit [
        i32 1, label %bad
      ]

    bad:
      %v = load i8, i8* null
      ret void

    exit:
      ret void
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

TEST_F(PulseCheckerTest, WideningKeepsPhiPredecessorWithRepresentativeState) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i8 @safe_three_way_merge(i32 %choice) {
    entry:
      %q = call i8* @malloc(i64 1)
      %r = call i8* @malloc(i64 1)
      switch i32 %choice, label %third [
        i32 0, label %first
        i32 1, label %second
      ]

    first:
      call void @free(i8* %q)
      br label %merge

    second:
      br label %merge

    third:
      br label %merge

    merge:
      %selected = phi i8* [ %r, %first ], [ %r, %second ], [ %q, %third ]
      %value = load i8, i8* %selected
      ret i8 %value
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::UseAfterFree);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), before);
}

TEST_F(PulseCheckerTest, SymbolicOutOfBoundsBranchIsReported) {
  auto module = parseModule(context, R"(
    define i32 @symbolic_oob(i64 %i) {
    entry:
      %a = alloca [2 x i32]
      %bad = icmp sge i64 %i, 2
      br i1 %bad, label %oob, label %ok

    oob:
      %p = getelementptr [2 x i32], [2 x i32]* %a, i64 0, i64 %i
      %v = load i32, i32* %p
      ret i32 %v

    ok:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::OutOfBounds);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::OutOfBounds), before + 1);
}

TEST_F(PulseCheckerTest, PartialArrayInitializationDoesNotInitializeAllElements) {
  auto module = parseModule(context, R"(
    define i32 @partial_init() {
    entry:
      %a = alloca [2 x i32]
      %p0 = getelementptr [2 x i32], [2 x i32]* %a, i64 0, i64 0
      store i32 1, i32* %p0
      %p1 = getelementptr [2 x i32], [2 x i32]* %a, i64 0, i64 1
      %v = load i32, i32* %p1
      ret i32 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before =
      getReportCountForType(mgr, IssueType::UninitializedRead);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UninitializedRead),
            before + 1);
}

TEST_F(PulseCheckerTest, ReadTaintsDestinationBuffer) {
  auto module = parseModule(context, R"(
    declare i64 @read(i32, i8*, i64)
    declare i32 @system(i8*)

    define void @read_to_system(i32 %fd) {
    entry:
      %buf = alloca [8 x i8]
      %p = bitcast [8 x i8]* %buf to i8*
      %n = call i64 @read(i32 %fd, i8* %p, i64 8)
      %r = call i32 @system(i8* %p)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  PulseChecker checker(module.get());
  Function *F = module->getFunction("read_to_system");
  ASSERT_NE(F, nullptr);
  auto *readCall = dyn_cast<CallInst>(findCallTo(F, "read"));
  auto *systemCall = dyn_cast<CallInst>(findCallTo(F, "system"));
  ASSERT_NE(readCall, nullptr);
  ASSERT_NE(systemCall, nullptr);
  ExecutionDomain state = checker.initializeFunction(F);
  for (auto &I : F->getEntryBlock()) {
    auto states = checker.executeInstruction(&I, std::move(state), nullptr, 0);
    ASSERT_FALSE(states.empty());
    state = std::move(states.front());
    if (&I == readCall)
      break;
  }
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);
  auto bufOpt = checker.getOperations().eval(
      *astate, readCall->getArgOperand(1), readCall, nullptr);
  ASSERT_TRUE(bufOpt.has_value());
  EXPECT_TRUE(astate->getTaintDomain().has(
      astate->getCanonical(bufOpt->addr)));
  EXPECT_TRUE(TaintOperations::checkSink(
      *astate, astate->getCanonical(bufOpt->addr), "system", systemCall));
}

TEST_F(PulseCheckerTest, FreeInvalidatesDerivedPointer) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)
    declare void @free(i8*)

    define i8 @derived_uaf() {
    entry:
      %p = call i8* @malloc(i64 4)
      %q = getelementptr i8, i8* %p, i64 1
      call void @free(i8* %p)
      %v = load i8, i8* %q
      ret i8 %v
    }
  )");
  ASSERT_NE(module, nullptr);

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const size_t before = getReportCountForType(mgr, IssueType::UseAfterFree);
  PulseChecker checker(module.get());
  checker.analyze();
  EXPECT_EQ(getReportCountForType(mgr, IssueType::UseAfterFree), before + 1);
}

TEST_F(PulseCheckerTest, IndirectCallIsNotModeledAsNoOp) {
  auto module = parseModule(context, R"(
    define void @indirect(void (i8**)* %fp, i8** %slot) {
    entry:
      call void %fp(i8** %slot)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("indirect");
  ASSERT_NE(F, nullptr);
  auto *call = findNthInstruction<CallInst>(F, 0);
  ASSERT_NE(call, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);
  Argument *slot = F->getArg(1);
  AbstractValue slotAv = checker.getFactory().getOrCreate(slot);
  AbstractValue pointee = checker.getFactory().createFresh();
  astate->getPostHeap().addEdge(slotAv, Access(AccessKind::Dereference),
                                Address(pointee));

  auto states = checker.executeInstruction(call, std::move(state), nullptr, 0);
  ASSERT_EQ(states.size(), 1u);
  astate = states.front().getAstate();
  ASSERT_NE(astate, nullptr);
  EXPECT_TRUE(astate->hasUnknownValues());
  EXPECT_TRUE(astate->hasSkippedCall("<indirect>"));
  EXPECT_EQ(astate->getPostHeap().findEdge(
                astate->getCanonical(slotAv), Access(AccessKind::Dereference)),
            nullptr);
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
