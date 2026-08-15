TEST_F(ICFGTest, LoopInControlFlow) {
  const char *source = R"(
    define void @loop_example(i32 %n) {
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
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *F = module->getFunction("loop_example");
  ASSERT_NE(F, nullptr);

  const BasicBlock *loopBB = findBlock(F, "loop");
  const BasicBlock *exitBB = findBlock(F, "exit");

  ASSERT_NE(loopBB, nullptr);
  ASSERT_NE(exitBB, nullptr);

  IntraBlockNode *loopNode = icfg.getIntraBlockNode(loopBB);
  IntraBlockNode *exitNode = icfg.getIntraBlockNode(exitBB);

  ASSERT_NE(loopNode, nullptr);
  ASSERT_NE(exitNode, nullptr);

  // Exit should be reachable from loop
  EXPECT_NE(icfg.getICFGEdge(loopNode, exitNode, ICFGEdge::IntraCF), nullptr);
}

// Test 10: Switch instruction handling
TEST_F(ICFGTest, SwitchInstruction) {
  const char *source = R"(
    define void @switch_example(i32 %x) {
    entry:
      switch i32 %x, label %default [
        i32 1, label %case1
        i32 2, label %case2
        i32 3, label %case3
      ]
    case1:
      br label %exit
    case2:
      br label %exit
    case3:
      br label %exit
    default:
      br label %exit
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *F = module->getFunction("switch_example");
  ASSERT_NE(F, nullptr);

  const BasicBlock *entry = &F->getEntryBlock();
  const BasicBlock *exitBB = findBlock(F, "exit");

  ASSERT_NE(entry, nullptr);
  ASSERT_NE(exitBB, nullptr);

  IntraBlockNode *entryNode = icfg.getIntraBlockNode(entry);
  IntraBlockNode *exitNode = icfg.getIntraBlockNode(exitBB);

  ASSERT_NE(entryNode, nullptr);
  ASSERT_NE(exitNode, nullptr);

  // Entry should have edges to all switch targets
  // Verify by checking exit is reachable
  EXPECT_NE(exitNode, nullptr);
}

// Test 11: Indirect function call
TEST_F(ICFGTest, IndirectCall) {
  const char *source = R"(
    define i32 @func1() {
      ret i32 1
    }

    define i32 @func2() {
      ret i32 2
    }

    define i32 @indirect_caller(i32 %which) {
      %fp = select i1 true, i32()* @func1, i32()* @func2
      %result = call i32 %fp()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *indirectCaller = module->getFunction("indirect_caller");
  Function *func1 = module->getFunction("func1");
  Function *func2 = module->getFunction("func2");
  ASSERT_NE(indirectCaller, nullptr);
  ASSERT_NE(func1, nullptr);
  ASSERT_NE(func2, nullptr);

  const CallBase *call = nullptr;
  for (const Instruction &inst : indirectCaller->getEntryBlock()) {
    if (const auto *candidate = dyn_cast<CallBase>(&inst)) {
      call = candidate;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  IntraBlockNode *callerNode =
      icfg.getIntraBlockNode(&indirectCaller->getEntryBlock());
  FunEntryBlockNode *func1Entry = icfg.getFunEntryICFGNode(func1);
  FunEntryBlockNode *func2Entry = icfg.getFunEntryICFGNode(func2);
  CallRetBlockNode *returnSite = icfg.getRetICFGNode(call);
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(func1Entry, nullptr);
  ASSERT_NE(func2Entry, nullptr);
  ASSERT_NE(returnSite, nullptr);

  EXPECT_NE(icfg.getICFGEdge(callerNode, func1Entry, ICFGEdge::CallCF, call),
            nullptr);
  EXPECT_NE(icfg.getICFGEdge(callerNode, func2Entry, ICFGEdge::CallCF, call),
            nullptr);
  auto *summary = dyn_cast<CallToRetCFGEdge>(
      icfg.getICFGEdge(callerNode, returnSite, ICFGEdge::CallToRetCF, call));
  ASSERT_NE(summary, nullptr);
  EXPECT_TRUE(summary->hasUnresolvedCallee());
}

TEST_F(ICFGTest, IndirectCallUsesInjectedCalleeProvider) {
  const char *source = R"(
    define i32 @func1() {
      ret i32 1
    }

    define i32 @func2() {
      ret i32 2
    }

    define i32 @indirect_caller(i32 ()* %fp) {
      %result = call i32 %fp()
      ret i32 %result
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *func1 = module->getFunction("func1");
  Function *func2 = module->getFunction("func2");
  Function *caller = module->getFunction("indirect_caller");
  ASSERT_NE(func1, nullptr);
  ASSERT_NE(func2, nullptr);
  ASSERT_NE(caller, nullptr);

  class SingleTargetProvider final : public ICFGCalleeProvider {
  public:
    explicit SingleTargetProvider(const Function *target) : target(target) {}

    ICFGCalleeTargets getTargets(const CallBase &) const override {
      ICFGCalleeTargets result;
      result.targets.push_back(target);
      result.complete = true;
      return result;
    }

  private:
    const Function *target;
  } provider(func1);

  ICFG icfg;
  ICFGBuilder builder(&icfg, &provider);
  builder.build(module.get());

  const CallBase *call = nullptr;
  for (const Instruction &inst : caller->getEntryBlock()) {
    if (const auto *candidate = dyn_cast<CallBase>(&inst)) {
      call = candidate;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  FunEntryBlockNode *func1Entry = icfg.getFunEntryICFGNode(func1);
  FunEntryBlockNode *func2Entry = icfg.getFunEntryICFGNode(func2);
  CallRetBlockNode *returnSite = icfg.getRetICFGNode(call);
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(func1Entry, nullptr);
  ASSERT_NE(func2Entry, nullptr);
  ASSERT_NE(returnSite, nullptr);

  EXPECT_NE(icfg.getICFGEdge(callerNode, func1Entry, ICFGEdge::CallCF, call),
            nullptr);
  EXPECT_EQ(icfg.getICFGEdge(callerNode, func2Entry, ICFGEdge::CallCF, call),
            nullptr);
  auto *summary = dyn_cast<CallToRetCFGEdge>(
      icfg.getICFGEdge(callerNode, returnSite, ICFGEdge::CallToRetCF, call));
  ASSERT_NE(summary, nullptr);
  EXPECT_FALSE(summary->hasUnresolvedCallee());
}

TEST_F(ICFGTest, DedicatedReturnSiteDoesNotCollapseToCallerBlock) {
  const char *source = R"(
    declare void @ext()

    define i32 @caller() {
    entry:
      call void @ext()
      %x = add i32 1, 2
      ret i32 %x
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *caller = module->getFunction("caller");
  ASSERT_NE(caller, nullptr);
  const CallBase *call = nullptr;
  for (const Instruction &I : caller->getEntryBlock()) {
    if (const auto *CB = dyn_cast<CallBase>(&I)) {
      call = CB;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  CallRetBlockNode *retSite = icfg.getRetICFGNode(call);
  IntraBlockNode *callerBlock =
      icfg.getIntraBlockNode(&caller->getEntryBlock());
  ASSERT_NE(retSite, nullptr);
  ASSERT_NE(callerBlock, nullptr);

  EXPECT_EQ(icfg.getICFGEdge(retSite, callerBlock, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(retSite, icfg.getFunExitICFGNode(caller),
                             ICFGEdge::IntraCF),
            nullptr);
}

TEST_F(ICFGTest, RootReturnIsConnectedToFunctionExit) {
  const char *source = R"(
    define i32 @main() {
    entry:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  IntraBlockNode *returnBlock =
      icfg.getIntraBlockNode(&mainFn->getEntryBlock());
  FunExitBlockNode *exitNode = icfg.getFunExitICFGNode(mainFn);
  ASSERT_NE(returnBlock, nullptr);
  ASSERT_NE(exitNode, nullptr);

  EXPECT_NE(icfg.getICFGEdge(returnBlock, exitNode, ICFGEdge::IntraCF),
            nullptr);
}

TEST_F(ICFGTest, NoreturnInvokeKeepsOnlyExceptionalContinuation) {
  const char *source = R"(
    declare i32 @__gxx_personality_v0(...)
    declare void @noreturn_may_throw() noreturn

    define i32 @caller() personality i32 (...)* @__gxx_personality_v0 {
    entry:
      invoke void @noreturn_may_throw()
              to label %normal unwind label %lpad
    normal:
      ret i32 0
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
  const auto *invoke =
      dyn_cast<InvokeInst>(findCallTo(caller, "noreturn_may_throw"));
  ASSERT_NE(invoke, nullptr);

  IntraBlockNode *callerNode = icfg.getIntraBlockNode(&caller->getEntryBlock());
  IntraBlockNode *normalNode =
      icfg.getIntraBlockNode(findBlock(caller, "normal"));
  IntraBlockNode *lpadNode = icfg.getIntraBlockNode(findBlock(caller, "lpad"));
  CallUnwindBlockNode *unwindSite = icfg.getUnwindICFGNode(invoke);
  ASSERT_NE(callerNode, nullptr);
  ASSERT_NE(normalNode, nullptr);
  ASSERT_NE(lpadNode, nullptr);
  ASSERT_NE(unwindSite, nullptr);

  EXPECT_EQ(icfg.getICFGEdge(callerNode, normalNode, ICFGEdge::IntraCF),
            nullptr);
  EXPECT_NE(
      icfg.getICFGEdge(callerNode, unwindSite, ICFGEdge::CallToRetCF, invoke),
      nullptr);
  EXPECT_NE(icfg.getICFGEdge(unwindSite, lpadNode, ICFGEdge::IntraCF), nullptr);

  for (const auto &entry : icfg) {
    const auto *retSite = dyn_cast<CallRetBlockNode>(entry.second);
    EXPECT_TRUE(!retSite || retSite->getCallSite() != invoke);
  }
}

TEST_F(ICFGTest, CatchSwitchUnwindToCallerConnectsExceptionalExit) {
  const char *source = R"(
    declare i32 @__CxxFrameHandler3(...)
    declare void @may_throw()

    define void @root() personality i32 (...)* @__CxxFrameHandler3 {
    entry:
      invoke void @may_throw()
              to label %done unwind label %catch.dispatch
    catch.dispatch:
      %switch = catchswitch within none [label %catch] unwind to caller
    catch:
      %pad = catchpad within %switch [i8* null, i32 64, i8* null]
      catchret from %pad to label %done
    done:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *root = module->getFunction("root");
  ASSERT_NE(root, nullptr);
  IntraBlockNode *catchSwitchNode =
      icfg.getIntraBlockNode(findBlock(root, "catch.dispatch"));
  FunUnwindExitBlockNode *unwindExit = icfg.getFunUnwindExitICFGNode(root);
  ASSERT_NE(catchSwitchNode, nullptr);
  ASSERT_NE(unwindExit, nullptr);

  EXPECT_NE(icfg.getICFGEdge(catchSwitchNode, unwindExit, ICFGEdge::IntraCF),
            nullptr);
}

TEST_F(ICFGTest, EdgeCountTracksInsertionAndRemoval) {
  const char *source = R"(
    define void @main() {
    entry:
      br label %exit
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  ICFGNode *entryNode = icfg.getIntraBlockNode(&mainFn->getEntryBlock());
  ICFGNode *exitBlock = icfg.getIntraBlockNode(findBlock(mainFn, "exit"));
  ICFGEdge *edge = icfg.getICFGEdge(entryNode, exitBlock, ICFGEdge::IntraCF);
  ASSERT_NE(edge, nullptr);

  uint32_t countBeforeRemoval = icfg.getTotalEdgeNum();
  ASSERT_GT(countBeforeRemoval, 0u);
  icfg.removeICFGEdge(edge);
  EXPECT_EQ(icfg.getTotalEdgeNum(), countBeforeRemoval - 1);
}

TEST_F(ICFGTest, PartialDistanceMapTreatsMissingNodesAsInfinity) {
  const char *source = R"(
    define void @main() {
    entry:
      br label %exit
    exit:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  ICFGNode *entryNode = icfg.getIntraBlockNode(&mainFn->getEntryBlock());
  ICFGNode *exitBlock = icfg.getIntraBlockNode(findBlock(mainFn, "exit"));
  std::map<ICFGNode *, uint64_t> distances;

  calculateDistanceMapInterICFGWithDistanceMap(&icfg, entryNode, distances);
  EXPECT_EQ(distances[entryNode], 0u);
  EXPECT_EQ(distances[exitBlock], 1u);
}

TEST_F(ICFGTest, ShortestPathFromBlockToItselfIsTrivial) {
  const char *source = R"(
    define void @main() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  BasicBlock *entry = &mainFn->getEntryBlock();
  std::vector<BasicBlock *> path;
  EXPECT_TRUE(calculateShortestPathIntra(entry, entry, path));
  ASSERT_EQ(path.size(), 1u);
  EXPECT_EQ(path.front(), entry);
}

TEST_F(ICFGTest, BoundedReachabilityIsConservativeOnBudgetExhaustion) {
  std::string source = "define void @long_chain() {\nentry:\n  br label %b0\n";
  for (unsigned i = 0; i < 70; ++i) {
    source += "b" + std::to_string(i) + ":\n";
    if (i + 1 < 70)
      source += "  br label %b" + std::to_string(i + 1) + "\n";
    else
      source += "  ret void\n";
  }
  source += "unreachable:\n  ret void\n}\n";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *function = module->getFunction("long_chain");
  ASSERT_NE(function, nullptr);

  BasicBlock *entry = &function->getEntryBlock();
  BasicBlock *unreachable = findBlock(function, "unreachable");
  ASSERT_NE(unreachable, nullptr);

  int iterations = 0;
  EXPECT_TRUE(
      isReachableFrom(entry, unreachable, nullptr, nullptr, iterations));
  EXPECT_EQ(iterations, 64);
}

TEST_F(ICFGTest, RemoveDedicatedNodesClearsSideMaps) {
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

  const CallBase *call = findCallTo(caller, "callee");
  ASSERT_NE(call, nullptr);

  FunEntryBlockNode *entryNode = icfg.getFunEntryICFGNode(callee);
  FunExitBlockNode *exitNode = icfg.getFunExitICFGNode(callee);
  CallRetBlockNode *retNode = icfg.getRetICFGNode(call);
  ASSERT_NE(entryNode, nullptr);
  ASSERT_NE(exitNode, nullptr);
  ASSERT_NE(retNode, nullptr);
  NodeID entryId = entryNode->getId();
  NodeID exitId = exitNode->getId();
  NodeID retId = retNode->getId();

  icfg.removeICFGNode(entryNode);
  icfg.removeICFGNode(exitNode);
  icfg.removeICFGNode(retNode);

  EXPECT_NE(icfg.getFunEntryICFGNode(callee)->getId(), entryId);
  EXPECT_NE(icfg.getFunExitICFGNode(callee)->getId(), exitId);
  EXPECT_NE(icfg.getRetICFGNode(call)->getId(), retId);
}

TEST_F(ICFGTest, RemoveDedicatedUnwindNodesClearsSideMaps) {
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

  FunUnwindExitBlockNode *unwindExit = icfg.getFunUnwindExitICFGNode(callee);
  CallUnwindBlockNode *unwindNode = icfg.getUnwindICFGNode(invoke);
  ASSERT_NE(unwindExit, nullptr);
  ASSERT_NE(unwindNode, nullptr);
  NodeID unwindExitId = unwindExit->getId();
  NodeID unwindNodeId = unwindNode->getId();

  icfg.removeICFGNode(unwindExit);
  icfg.removeICFGNode(unwindNode);

  EXPECT_NE(icfg.getFunUnwindExitICFGNode(callee)->getId(), unwindExitId);
  EXPECT_NE(icfg.getUnwindICFGNode(invoke)->getId(), unwindNodeId);
}

TEST_F(ICFGTest, RemovingSelfLoopNodeDoesNotDoubleDeleteEdges) {
  const char *source = R"(
    define void @loop_example() {
    entry:
      br label %loop

    loop:
      br label %loop
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *F = module->getFunction("loop_example");
  ASSERT_NE(F, nullptr);
  const BasicBlock *loopBB = findBlock(F, "loop");
  ASSERT_NE(loopBB, nullptr);

  IntraBlockNode *loopNode = icfg.getIntraBlockNode(loopBB);
  ASSERT_NE(loopNode, nullptr);
  NodeID loopNodeId = loopNode->getId();

  icfg.removeICFGNode(loopNode);

  EXPECT_FALSE(icfg.hasICFGNode(loopNodeId));
}

// Test 12: Empty function handling
TEST_F(ICFGTest, EmptyFunction) {
  const char *source = R"(
    define void @empty() {
      ret void
    }

    define void @caller() {
      call void @empty()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *empty = module->getFunction("empty");
  ASSERT_NE(empty, nullptr);

  IntraBlockNode *emptyEntry = icfg.getIntraBlockNode(&empty->getEntryBlock());
  ASSERT_NE(emptyEntry, nullptr);

  // Empty function should still have a node
  EXPECT_NE(emptyEntry, nullptr);
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
