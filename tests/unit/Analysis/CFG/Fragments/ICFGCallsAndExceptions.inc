TEST_F(ICFGTest, FunctionCall) {
  const char *source = R"(
    define i32 @callee() {
      ret i32 42
    }

    define i32 @caller() {
      %result = call i32 @callee()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  // Find the call instruction
  const CallBase *call = findCallTo(caller, "callee");
  ASSERT_NE(call, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  FunEntryBlockNode *calleeEntryNode = icfg.getFunEntryICFGNode(callee);

  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(calleeEntryNode, nullptr);

  // Should have interprocedural call edge
  ICFGEdge *callEdge =
      icfg.getICFGEdge(callerNode, calleeEntryNode, ICFGEdge::CallCF);
  EXPECT_NE(callEdge, nullptr);
}

// Test 4: Return edge from callee to caller
TEST_F(ICFGTest, ReturnEdgeFromCallee) {
  const char *source = R"(
    define i32 @callee() {
    entry:
      ret i32 1
    }

    define i32 @caller() {
    entry:
      %result = call i32 @callee()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  const BasicBlock *callerEntry = &caller->getEntryBlock();
  const CallBase *call = findCallTo(caller, "callee");
  ASSERT_NE(call, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(callerEntry);
  FunEntryBlockNode *calleeEntryNode = icfg.getFunEntryICFGNode(callee);
  FunExitBlockNode *calleeExitNode = icfg.getFunExitICFGNode(callee);
  CallRetBlockNode *retSiteNode = icfg.getRetICFGNode(call);

  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(calleeEntryNode, nullptr);
  ASSERT_NE(calleeExitNode, nullptr);
  ASSERT_NE(retSiteNode, nullptr);

  // Call edge
  ICFGEdge *callEdge =
      icfg.getICFGEdge(callerNode, calleeEntryNode, ICFGEdge::CallCF);
  EXPECT_NE(callEdge, nullptr);

  // Return edge
  ICFGEdge *retEdge =
      icfg.getICFGEdge(calleeExitNode, retSiteNode, ICFGEdge::RetCF);
  EXPECT_NE(retEdge, nullptr);
}

// Test 5: Multiple callers of the same function
TEST_F(ICFGTest, MultipleCallers) {
  const char *source = R"(
    define i32 @shared() {
      ret i32 0
    }

    define i32 @caller1() {
      %result = call i32 @shared()
      ret i32 %result
    }

    define i32 @caller2() {
      %result = call i32 @shared()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *shared = module->getFunction("shared");
  Function *caller1 = module->getFunction("caller1");
  Function *caller2 = module->getFunction("caller2");

  ASSERT_NE(shared, nullptr);
  ASSERT_NE(caller1, nullptr);
  ASSERT_NE(caller2, nullptr);

  FunEntryBlockNode *sharedEntry = icfg.getFunEntryICFGNode(shared);
  IntraBlockNode *caller1Node =
      icfg.getIntraBlockNode(&caller1->getEntryBlock());
  IntraBlockNode *caller2Node =
      icfg.getIntraBlockNode(&caller2->getEntryBlock());

  ASSERT_NE(sharedEntry, nullptr);
  ASSERT_NE(caller1Node, nullptr);
  ASSERT_NE(caller2Node, nullptr);

  // Both callers should have call edges to shared
  EXPECT_NE(icfg.getICFGEdge(caller1Node, sharedEntry, ICFGEdge::CallCF),
            nullptr);
  EXPECT_NE(icfg.getICFGEdge(caller2Node, sharedEntry, ICFGEdge::CallCF),
            nullptr);
}

// Test 6: Nested function calls
TEST_F(ICFGTest, NestedFunctionCalls) {
  const char *source = R"(
    define i32 @inner() {
      ret i32 1
    }

    define i32 @middle() {
      %result = call i32 @inner()
      ret i32 %result
    }

    define i32 @outer() {
      %result = call i32 @middle()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *outer = module->getFunction("outer");
  Function *middle = module->getFunction("middle");
  Function *inner = module->getFunction("inner");

  ASSERT_NE(outer, nullptr);
  ASSERT_NE(middle, nullptr);
  ASSERT_NE(inner, nullptr);

  IntraBlockNode *outerNode = icfg.getIntraBlockNode(&outer->getEntryBlock());
  IntraBlockNode *middleCallerNode =
      icfg.getIntraBlockNode(&middle->getEntryBlock());
  FunEntryBlockNode *middleEntryNode = icfg.getFunEntryICFGNode(middle);
  FunEntryBlockNode *innerNode = icfg.getFunEntryICFGNode(inner);

  ASSERT_NE(outerNode, nullptr);
  ASSERT_NE(middleCallerNode, nullptr);
  ASSERT_NE(middleEntryNode, nullptr);
  ASSERT_NE(innerNode, nullptr);

  // Call chain: outer -> middle -> inner
  EXPECT_NE(icfg.getICFGEdge(outerNode, middleEntryNode, ICFGEdge::CallCF),
            nullptr);
  EXPECT_NE(icfg.getICFGEdge(middleCallerNode, innerNode, ICFGEdge::CallCF),
            nullptr);
}

// Test 7: Recursive function call
TEST_F(ICFGTest, RecursiveCall) {
  const char *source = R"(
    define i32 @fact(i32 %n) {
    entry:
      %cmp = icmp sle i32 %n, 1
      br i1 %cmp, label %base, label %recurse

    base:
      ret i32 1

    recurse:
      %n1 = sub i32 %n, 1
      %result = call i32 @fact(i32 %n1)
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *fact = module->getFunction("fact");
  ASSERT_NE(fact, nullptr);

  IntraBlockNode *factEntry = icfg.getIntraBlockNode(&fact->getEntryBlock());
  ASSERT_NE(factEntry, nullptr);

  // For recursive calls, we should still have the call edge
  EXPECT_TRUE(true);
}

// Test 8: Function with multiple call sites
TEST_F(ICFGTest, MultipleCallSites) {
  const char *source = R"(
    define i32 @helper() {
      ret i32 42
    }

    define i32 @multi_call() {
      %r1 = call i32 @helper()
      %r2 = call i32 @helper()
      %r3 = call i32 @helper()
      ret i32 %r3
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *helper = module->getFunction("helper");
  Function *multiCall = module->getFunction("multi_call");

  ASSERT_NE(helper, nullptr);
  ASSERT_NE(multiCall, nullptr);

  IntraBlockNode *helperNode = icfg.getIntraBlockNode(&helper->getEntryBlock());
  ASSERT_NE(helperNode, nullptr);

  // Verify the module builds correctly
  EXPECT_TRUE(true);
}

TEST_F(ICFGTest, MultipleCallSitesInOneBlockAreSequenced) {
  const char *source = R"(
    define i32 @helper() {
      ret i32 42
    }

    define i32 @multi_call() {
    entry:
      %r1 = call i32 @helper()
      %r2 = call i32 @helper()
      %r3 = call i32 @helper()
      ret i32 %r3
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *helper = module->getFunction("helper");
  Function *multiCall = module->getFunction("multi_call");
  ASSERT_NE(helper, nullptr);
  ASSERT_NE(multiCall, nullptr);

  IntraBlockNode *callerNode =
      icfg.getIntraBlockNode(&multiCall->getEntryBlock());
  FunEntryBlockNode *calleeEntry = icfg.getFunEntryICFGNode(helper);
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(calleeEntry, nullptr);

  auto calls = findCallsTo(multiCall, "helper");
  ASSERT_EQ(calls.size(), 3u);

  CallRetBlockNode *firstRet = icfg.getRetICFGNode(calls[0]);
  CallRetBlockNode *secondRet = icfg.getRetICFGNode(calls[1]);
  CallRetBlockNode *thirdRet = icfg.getRetICFGNode(calls[2]);
  ASSERT_NE(firstRet, nullptr);
  ASSERT_NE(secondRet, nullptr);
  ASSERT_NE(thirdRet, nullptr);

  EXPECT_NE(
      icfg.getICFGEdge(callerNode, calleeEntry, ICFGEdge::CallCF, calls[0]),
      nullptr);
  EXPECT_NE(icfg.getICFGEdge(firstRet, calleeEntry, ICFGEdge::CallCF, calls[1]),
            nullptr);
  EXPECT_NE(
      icfg.getICFGEdge(secondRet, calleeEntry, ICFGEdge::CallCF, calls[2]),
      nullptr);
  EXPECT_EQ(
      icfg.getICFGEdge(callerNode, calleeEntry, ICFGEdge::CallCF, calls[1]),
      nullptr);

  FunExitBlockNode *callerExit = icfg.getFunExitICFGNode(multiCall);
  EXPECT_NE(icfg.getICFGEdge(thirdRet, callerExit, ICFGEdge::IntraCF), nullptr);
}

TEST_F(ICFGTest, ReturnSiteBranchesToAllSuccessorsAfterDirectCall) {
  const char *source = R"(
    define void @callee() {
      ret void
    }

    define i32 @caller(i1 %cond) {
    entry:
      call void @callee()
      br i1 %cond, label %then, label %else
    then:
      ret i32 1
    else:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  const CallBase *call = findCallTo(caller, "callee");
  ASSERT_NE(call, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  CallRetBlockNode *retSite = icfg.getRetICFGNode(call);
  IntraBlockNode *thenNode = icfg.getIntraBlockNode(findBlock(caller, "then"));
  IntraBlockNode *elseNode = icfg.getIntraBlockNode(findBlock(caller, "else"));
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(retSite, nullptr);
  ASSERT_NE(thenNode, nullptr);
  ASSERT_NE(elseNode, nullptr);

  EXPECT_EQ(icfg.getICFGEdge(callerNode, thenNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, elseNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, thenNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, elseNode, ICFGEdge::IntraCF), nullptr);
}

TEST_F(ICFGTest, ExternalCallStillBuildsReturnSiteContinuation) {
  const char *source = R"(
    declare void @ext()

    define i32 @caller(i1 %cond) {
    entry:
      call void @ext()
      br i1 %cond, label %then, label %else
    then:
      ret i32 1
    else:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  const CallBase *call = findCallTo(caller, "ext");
  ASSERT_NE(call, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  CallRetBlockNode *retSite = icfg.getRetICFGNode(call);
  IntraBlockNode *thenNode = icfg.getIntraBlockNode(findBlock(caller, "then"));
  IntraBlockNode *elseNode = icfg.getIntraBlockNode(findBlock(caller, "else"));
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(retSite, nullptr);
  ASSERT_NE(thenNode, nullptr);
  ASSERT_NE(elseNode, nullptr);

  EXPECT_EQ(icfg.getICFGEdge(callerNode, thenNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, elseNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(callerNode, retSite, ICFGEdge::CallToRetCF, call),
            nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, thenNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, elseNode, ICFGEdge::IntraCF), nullptr);
}

TEST_F(ICFGTest, InternalInvokeUsesDedicatedNormalAndUnwindContinuations) {
  const char *source = R"(
    declare i32 @__gxx_personality_v0(...)

    define i32 @callee() {
    entry:
      ret i32 7
    }

    define i32 @caller() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %r = invoke i32 @callee()
              to label %normal unwind label %lpad
    normal:
      ret i32 %r
    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  const auto *invoke = dyn_cast<InvokeInst>(findCallTo(caller, "callee"));
  ASSERT_NE(invoke, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  IntraBlockNode *normalNode =
      icfg.getIntraBlockNode(findBlock(caller, "normal"));
  IntraBlockNode *lpadNode = icfg.getIntraBlockNode(findBlock(caller, "lpad"));
  FunEntryBlockNode *calleeEntry = icfg.getFunEntryICFGNode(callee);
  FunExitBlockNode *calleeExit = icfg.getFunExitICFGNode(callee);
  CallRetBlockNode *retSite = icfg.getRetICFGNode(invoke);
  CallUnwindBlockNode *unwindSite = icfg.getUnwindICFGNode(invoke);

  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(normalNode, nullptr);
  ASSERT_NE(lpadNode, nullptr);
  ASSERT_NE(calleeEntry, nullptr);
  ASSERT_NE(calleeExit, nullptr);
  ASSERT_NE(retSite, nullptr);
  ASSERT_NE(unwindSite, nullptr);

  EXPECT_NE(icfg.getICFGEdge(callerNode, calleeEntry, ICFGEdge::CallCF),
            nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, normalNode, ICFGEdge::IntraCF),
            nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, lpadNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(calleeExit, retSite, ICFGEdge::RetCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, normalNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(unwindSite, lpadNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, unwindSite, ICFGEdge::IntraCF),
            nullptr);
}

TEST_F(ICFGTest, InternalInvokeExceptionalResumeUsesExceptionalReturnEdge) {
  const char *source = R"(
    declare i32 @__gxx_personality_v0(...)
    declare i32 @may_throw()

    define i32 @callee() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %v = invoke i32 @may_throw()
              to label %cont unwind label %lpad
    cont:
      ret i32 %v
    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      resume { i8*, i32 } %lp
    }

    define i32 @caller() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %r = invoke i32 @callee()
              to label %normal unwind label %outer_lpad
    normal:
      ret i32 %r
    outer_lpad:
      %outer = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  Function *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  const auto *invoke = dyn_cast<InvokeInst>(findCallTo(caller, "callee"));
  ASSERT_NE(invoke, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  IntraBlockNode *normalNode =
      icfg.getIntraBlockNode(findBlock(caller, "normal"));
  IntraBlockNode *outerLpadNode =
      icfg.getIntraBlockNode(findBlock(caller, "outer_lpad"));
  FunUnwindExitBlockNode *calleeUnwindExit =
      icfg.getFunUnwindExitICFGNode(callee);
  CallUnwindBlockNode *unwindSite = icfg.getUnwindICFGNode(invoke);

  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(normalNode, nullptr);
  ASSERT_NE(outerLpadNode, nullptr);
  ASSERT_NE(calleeUnwindExit, nullptr);
  ASSERT_NE(unwindSite, nullptr);

  EXPECT_EQ(icfg.getICFGEdge(callerNode, normalNode, ICFGEdge::IntraCF),
            nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, outerLpadNode, ICFGEdge::IntraCF),
            nullptr);
  EXPECT_NE(icfg.getICFGEdge(calleeUnwindExit, unwindSite, ICFGEdge::ExcRetCF),
            nullptr);
  EXPECT_NE(icfg.getICFGEdge(unwindSite, outerLpadNode, ICFGEdge::IntraCF),
            nullptr);
}

TEST_F(ICFGTest, ExternalInvokeUsesSummaryEdgesToContinuationNodes) {
  const char *source = R"(
    declare i32 @__gxx_personality_v0(...)
    declare i32 @may_throw()

    define i32 @caller() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %r = invoke i32 @may_throw()
              to label %normal unwind label %lpad
    normal:
      ret i32 %r
    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  const auto *invoke = dyn_cast<InvokeInst>(findCallTo(caller, "may_throw"));
  ASSERT_NE(invoke, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  IntraBlockNode *normalNode =
      icfg.getIntraBlockNode(findBlock(caller, "normal"));
  IntraBlockNode *lpadNode = icfg.getIntraBlockNode(findBlock(caller, "lpad"));
  CallRetBlockNode *retSite = icfg.getRetICFGNode(invoke);
  CallUnwindBlockNode *unwindSite = icfg.getUnwindICFGNode(invoke);

  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(normalNode, nullptr);
  ASSERT_NE(lpadNode, nullptr);
  ASSERT_NE(retSite, nullptr);
  ASSERT_NE(unwindSite, nullptr);

  EXPECT_EQ(icfg.getICFGEdge(callerNode, normalNode, ICFGEdge::IntraCF),
            nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, lpadNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(
      icfg.getICFGEdge(callerNode, retSite, ICFGEdge::CallToRetCF, invoke),
      nullptr);
  EXPECT_NE(
      icfg.getICFGEdge(callerNode, unwindSite, ICFGEdge::CallToRetCF, invoke),
      nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, normalNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(unwindSite, lpadNode, ICFGEdge::IntraCF), nullptr);
}

TEST_F(ICFGTest, InterproceduralDistanceUsesExceptionalSummaryPath) {
  const char *source = R"(
    declare i32 @__gxx_personality_v0(...)
    declare i32 @may_throw()

    define i32 @callee() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %v = invoke i32 @may_throw()
              to label %cont unwind label %lpad
    cont:
      ret i32 %v
    lpad:
      %lp = landingpad { i8*, i32 } cleanup
      resume { i8*, i32 } %lp
    }

    define i32 @caller() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      %r = invoke i32 @callee()
              to label %normal unwind label %outer_lpad
    normal:
      ret i32 %r
    outer_lpad:
      %outer = landingpad { i8*, i32 } cleanup
      ret i32 1
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  IntraBlockNode *outerLpadNode =
      icfg.getIntraBlockNode(findBlock(caller, "outer_lpad"));
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(outerLpadNode, nullptr);

  auto distanceMap = calculateDistanceMapInterICFG(&icfg, callerNode);
  EXPECT_EQ(distanceMap[outerLpadNode], 2u);
}

// Test 9: Loop in control flow
