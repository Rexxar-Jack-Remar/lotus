TEST_F(MHPAnalysisTest, ThreadFlowGraphNodes) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define i8* @worker(i8* %arg) {
      %l = call i32 @pthread_mutex_lock(i8* @lock)
      %u = call i32 @pthread_mutex_unlock(i8* @lock)
      ret i8* null
    }

    define i32 @main() {
      %tid = alloca i8
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      %join = call i32 @pthread_join(i8* %tid, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const ThreadFlowGraph &tfg = mhp.getThreadFlowGraph();
  auto forkNodes = tfg.getNodesOfType(SyncNodeType::THREAD_FORK);
  auto joinNodes = tfg.getNodesOfType(SyncNodeType::THREAD_JOIN);
  auto lockNodes = tfg.getNodesOfType(SyncNodeType::LOCK_ACQUIRE);
  auto unlockNodes = tfg.getNodesOfType(SyncNodeType::LOCK_RELEASE);

  EXPECT_GE(forkNodes.size(), 1u);
  EXPECT_GE(joinNodes.size(), 1u);
  EXPECT_GE(lockNodes.size(), 1u);
  EXPECT_GE(unlockNodes.size(), 1u);
}
TEST_F(MHPAnalysisTest, ThreadFlowGraphIndexesSCCCondensation) {
  ThreadFlowGraph tfg;
  tfg.addThread(0, nullptr);

  SyncNode *a = tfg.createNode(nullptr, SyncNodeType::REGULAR_INST, 0);
  SyncNode *c = tfg.createNode(nullptr, SyncNodeType::REGULAR_INST, 0);
  SyncNode *b = tfg.createNode(nullptr, SyncNodeType::REGULAR_INST, 0);
  tfg.addIntraThreadEdge(a, b);
  tfg.addIntraThreadEdge(b, b);
  tfg.addIntraThreadEdge(b, c);

  EXPECT_TRUE(tfg.canReach(b, c));
  tfg.buildReachabilityIndex();
  EXPECT_TRUE(tfg.canReach(b, c));
  EXPECT_LE(tfg.getTopologicalOrder(b), tfg.getTopologicalOrder(c));
}
TEST_F(MHPAnalysisTest, ReachabilityIndexPreservesInterThreadDetours) {
  ThreadFlowGraph tfg;
  tfg.addThread(0, nullptr);
  tfg.addThread(1, nullptr);

  SyncNode *a = tfg.createNode(nullptr, SyncNodeType::REGULAR_INST, 0);
  SyncNode *x = tfg.createNode(nullptr, SyncNodeType::REGULAR_INST, 1);
  SyncNode *b = tfg.createNode(nullptr, SyncNodeType::REGULAR_INST, 0);
  tfg.addInterThreadEdge(a, x, EdgeKind::Create);
  tfg.addInterThreadEdge(x, b, EdgeKind::Join);

  EXPECT_TRUE(tfg.canReach(a, b));
  tfg.buildReachabilityIndex();
  EXPECT_TRUE(tfg.canReach(a, b));
}
TEST_F(MHPAnalysisTest, ThreadFlowGraphMutationIsCanonical) {
  ThreadFlowGraph first;
  first.addThread(0, nullptr);
  SyncNode *a = first.createNode(nullptr, SyncNodeType::REGULAR_INST, 0);
  SyncNode *b = first.createNode(nullptr, SyncNodeType::REGULAR_INST, 0);
  SyncNode *c = first.createNode(nullptr, SyncNodeType::REGULAR_INST, 0);

  first.addIntraThreadEdge(a, b);
  first.addIntraThreadEdge(a, b);
  EXPECT_EQ(a->getSuccessors().size(), 1u);
  EXPECT_EQ(b->getPredecessors().size(), 1u);
  EXPECT_FALSE(first.getEdgeKind(b, c).has_value());

  first.buildReachabilityIndex();
  EXPECT_TRUE(first.hasReachabilityIndex());
  first.addIntraThreadEdge(c, a);
  EXPECT_FALSE(first.hasReachabilityIndex());
  EXPECT_TRUE(first.canReach(c, b));

  ThreadFlowGraph second;
  second.addThread(0, nullptr);
  SyncNode *second_a =
      second.createNode(nullptr, SyncNodeType::REGULAR_INST, 0);
  SyncNode *second_b =
      second.createNode(nullptr, SyncNodeType::REGULAR_INST, 0);
  EXPECT_EQ(a->getNodeID(), second_a->getNodeID());
  EXPECT_EQ(b->getNodeID(), second_b->getNodeID());
  EXPECT_EQ(first.getNodeByID(a->getNodeID()), a);
  EXPECT_EQ(first.getNodeByID(b->getNodeID()), b);
  EXPECT_EQ(first.getNodeByID(0), nullptr);
}
