TEST_F(ICFGTest, SimpleFunction) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %x = add i32 1, 2
      br label %exit
    exit:
      ret i32 %x
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *F = module->getFunction("main");
  ASSERT_NE(F, nullptr);

  // Should have nodes for each basic block
  unsigned nodeCount = 0;
  for (const auto &BB : *F) {
    IntraBlockNode *node = icfg.getIntraBlockNode(&BB);
    ASSERT_NE(node, nullptr);
    ++nodeCount;
  }

  EXPECT_EQ(nodeCount, 2u);
}

TEST_F(ICFGTest, GlobalInitNodeAnchorsMainWhenPresent) {
  const char *source = R"(
    define void @helper() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      call void @helper()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *mainFn = module->getFunction("main");
  Function *helperFn = module->getFunction("helper");
  ASSERT_NE(mainFn, nullptr);
  ASSERT_NE(helperFn, nullptr);

  GlobalInitBlockNode *globalInit = icfg.getGlobalInitICFGNode();
  ASSERT_NE(globalInit, nullptr);
  EXPECT_EQ(globalInit, icfg.getGlobalInitICFGNode());

  FunEntryBlockNode *mainEntry = icfg.getFunEntryICFGNode(mainFn);
  FunEntryBlockNode *helperEntry = icfg.getFunEntryICFGNode(helperFn);
  ASSERT_NE(mainEntry, nullptr);
  ASSERT_NE(helperEntry, nullptr);

  EXPECT_NE(icfg.getICFGEdge(globalInit, mainEntry, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(globalInit, helperEntry, ICFGEdge::IntraCF), nullptr);
}

TEST_F(ICFGTest, GlobalInitNodeConnectsAllRootsWithoutMain) {
  const char *source = R"(
    define void @leaf() {
    entry:
      ret void
    }

    define void @foo() {
    entry:
      call void @leaf()
      ret void
    }

    define void @bar() {
    entry:
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *fooFn = module->getFunction("foo");
  Function *barFn = module->getFunction("bar");
  Function *leafFn = module->getFunction("leaf");
  ASSERT_NE(fooFn, nullptr);
  ASSERT_NE(barFn, nullptr);
  ASSERT_NE(leafFn, nullptr);

  GlobalInitBlockNode *globalInit = icfg.getGlobalInitICFGNode();
  FunEntryBlockNode *fooEntry = icfg.getFunEntryICFGNode(fooFn);
  FunEntryBlockNode *barEntry = icfg.getFunEntryICFGNode(barFn);
  FunEntryBlockNode *leafEntry = icfg.getFunEntryICFGNode(leafFn);
  ASSERT_NE(globalInit, nullptr);
  ASSERT_NE(fooEntry, nullptr);
  ASSERT_NE(barEntry, nullptr);
  ASSERT_NE(leafEntry, nullptr);

  EXPECT_NE(icfg.getICFGEdge(globalInit, fooEntry, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(globalInit, barEntry, ICFGEdge::IntraCF), nullptr);
  EXPECT_EQ(icfg.getICFGEdge(globalInit, leafEntry, ICFGEdge::IntraCF), nullptr);
}

// Test 2: Intraprocedural edges for branch
TEST_F(ICFGTest, IntraEdgeCountForBranch) {
  const char *source = R"(
    define i32 @main(i32 %cond) {
    entry:
      %cmp = icmp eq i32 %cond, 0
      br i1 %cmp, label %then, label %else
    then:
      br label %exit
    else:
      br label %exit
    exit:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *F = module->getFunction("main");
  ASSERT_NE(F, nullptr);

  const BasicBlock *entry = &F->getEntryBlock();
  const BasicBlock *thenBB = findBlock(F, "then");
  const BasicBlock *elseBB = findBlock(F, "else");
  const BasicBlock *exitBB = findBlock(F, "exit");

  ASSERT_NE(thenBB, nullptr);
  ASSERT_NE(elseBB, nullptr);
  ASSERT_NE(exitBB, nullptr);

  IntraBlockNode *entryNode = icfg.getIntraBlockNode(entry);
  IntraBlockNode *thenNode = icfg.getIntraBlockNode(thenBB);
  IntraBlockNode *elseNode = icfg.getIntraBlockNode(elseBB);
  IntraBlockNode *exitNode = icfg.getIntraBlockNode(exitBB);

  ASSERT_NE(entryNode, nullptr);
  ASSERT_NE(thenNode, nullptr);
  ASSERT_NE(elseNode, nullptr);
  ASSERT_NE(exitNode, nullptr);

  // Check intraprocedural edges
  EXPECT_NE(icfg.getICFGEdge(entryNode, thenNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(entryNode, elseNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(thenNode, exitNode, ICFGEdge::IntraCF), nullptr);
  EXPECT_NE(icfg.getICFGEdge(elseNode, exitNode, ICFGEdge::IntraCF), nullptr);
}

// Test 3: Interprocedural edges for function calls
