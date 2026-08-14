TEST_F(PulseCheckerTest, SummaryKeepsReturnValueFromMatchingExitPath) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define i8* @choose_ret(i8* %p) {
    entry:
      %isnull = icmp eq i8* %p, null
      br i1 %isnull, label %ret_null, label %ret_arg

    ret_null:
      ret i8* null

    ret_arg:
      ret i8* %p
    }

    define void @caller() {
    entry:
      %p = call i8* @malloc(i64 1)
      %q = call i8* @choose_ret(i8* %p)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  auto *mallocCall = dyn_cast<CallInst>(findCallTo(caller, "malloc"));
  auto *chooseCall = dyn_cast<CallInst>(findCallTo(caller, "choose_ret"));
  ASSERT_NE(mallocCall, nullptr);
  ASSERT_NE(chooseCall, nullptr);

  PulseChecker checker(module.get());
  checker.analyze();

  ExecutionDomain state = executeEntryBlock(checker, caller, chooseCall);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);

  const Address *mallocAddr = astate->getPostStack().find(mallocCall);
  const Address *callAddr = astate->getPostStack().find(chooseCall);
  ASSERT_NE(mallocAddr, nullptr);
  ASSERT_NE(callAddr, nullptr);

  EXPECT_EQ(astate->getCanonical(mallocAddr->addr),
            astate->getCanonical(callAddr->addr));
  EXPECT_FALSE(
      astate->getPostAttrs().has(astate->getCanonical(callAddr->addr),
                                 pulse::Attribute::Null));
}

TEST_F(PulseCheckerTest, RejectedMultiEntrySummaryFallsBackToUnknownCall) {
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define i8* @pick_first(i8* %a, i8* %b, i1 %flag) {
    entry:
      %distinct = icmp ne i8* %a, %b
      br i1 %distinct, label %ok, label %reject

    ok:
      br i1 %flag, label %ret1, label %ret2

    ret1:
      ret i8* %a

    ret2:
      ret i8* %a

    reject:
      unreachable
    }

    define void @caller_same_ptr() {
    entry:
      %p = call i8* @malloc(i64 1)
      %q = call i8* @pick_first(i8* %p, i8* %p, i1 false)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *caller = module->getFunction("caller_same_ptr");
  ASSERT_NE(caller, nullptr);
  auto *pickCall = dyn_cast<CallInst>(findCallTo(caller, "pick_first"));
  ASSERT_NE(pickCall, nullptr);

  PulseChecker checker(module.get());
  checker.analyze();

  ExecutionDomain state = executeEntryBlock(checker, caller, pickCall);
  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);
  EXPECT_TRUE(astate->hasSkippedCall("pick_first"));
}

TEST_F(PulseCheckerTest, SummaryApplicationHandlesAliasedActuals) {
  auto module = parseModule(context, R"(
    define void @store_two(i8** %a, i8** %b, i8* %v) {
    entry:
      store i8* %v, i8** %a
      store i8* %v, i8** %b
      ret void
    }

    define void @caller(i8** %p, i8* %v) {
    entry:
      call void @store_two(i8** %p, i8** %p, i8* %v)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *callee = module->getFunction("store_two");
  auto *caller = module->getFunction("caller");
  ASSERT_NE(callee, nullptr);
  ASSERT_NE(caller, nullptr);

  PulseChecker checker(module.get());
  checker.analyzeFunction(callee);

  ExecutionDomain callerState = checker.initializeFunction(caller);
  auto *call = dyn_cast<CallInst>(findCallTo(caller, "store_two"));
  ASSERT_NE(call, nullptr);

  auto applied = checker.applySummaryImproved(callee, callerState, call, nullptr);
  ASSERT_FALSE(applied.empty());
}

TEST_F(PulseCheckerTest, UnknownCallHavocsPointerArguments) {
  auto module = parseModule(context, R"(
    declare void @mystery(i8**)

    define void @caller() {
    entry:
      %slot = alloca i8*
      %buf = alloca i8
      store i8* %buf, i8** %slot
      call void @mystery(i8** %slot)
      %p = load i8*, i8** %slot
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("caller");
  ASSERT_NE(F, nullptr);

  PulseChecker checker(module.get());
  ExecutionDomain state = checker.initializeFunction(F);
  auto *call = dyn_cast<CallInst>(findCallTo(F, "mystery"));
  ASSERT_NE(call, nullptr);

  for (auto &I : F->getEntryBlock()) {
    auto states = checker.executeInstruction(&I, std::move(state), nullptr, 0);
    ASSERT_FALSE(states.empty());
    state = std::move(states.front());
    if (&I == call) {
      break;
    }
  }

  auto *astate = state.getAstate();
  ASSERT_NE(astate, nullptr);
  auto *slot = findNthInstruction<AllocaInst>(F, 0);
  ASSERT_NE(slot, nullptr);
  AbstractValue slot_av = checker.getFactory().getOrCreate(slot);
  EXPECT_EQ(astate->getPostHeap().findEdge(astate->getCanonical(slot_av),
                                           Access(AccessKind::Dereference)),
            nullptr);
}
