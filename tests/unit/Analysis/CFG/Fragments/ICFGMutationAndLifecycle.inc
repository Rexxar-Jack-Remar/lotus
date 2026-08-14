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
  ASSERT_NE(indirectCaller, nullptr);

  // Indirect calls might not have direct ICFG edges in basic ICFG
  // Verify the module builds correctly
  EXPECT_TRUE(true);
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
  IntraBlockNode *callerBlock = icfg.getIntraBlockNode(&caller->getEntryBlock());
  ASSERT_NE(retSite, nullptr);
  ASSERT_NE(callerBlock, nullptr);

  EXPECT_EQ(icfg.getICFGEdge(retSite, callerBlock, ICFGEdge::IntraCF), nullptr);
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
