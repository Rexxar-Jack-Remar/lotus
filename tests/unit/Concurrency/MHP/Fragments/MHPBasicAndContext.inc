TEST_F(MHPAnalysisTest, SimpleMain) {
  const char *source = R"(
    define i32 @main() {
      %x = add i32 1, 2
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  EXPECT_NO_THROW(mhp.analyze());

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_threads, 0);
}
TEST_F(MHPAnalysisTest, ThreadCreation) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)

    define i8* @worker(i8* %arg) {
      ret i8* null
    }

    define i32 @main() {
      %tid = alloca i8
      %ret = call i32 @pthread_create(i8* %tid, i8* null,
                                       i8* (i8*)* @worker, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_forks, 1);
}
TEST_F(MHPAnalysisTest, LockOperations) {
  const char *source = R"(
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)

    @lock = global i8 0

    define i32 @main() {
      %l = call i32 @pthread_mutex_lock(i8* @lock)
      %x = add i32 1, 2
      %u = call i32 @pthread_mutex_unlock(i8* @lock)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_locks, 1);
  EXPECT_GE(stats.num_unlocks, 1);
}
TEST_F(MHPAnalysisTest, WrapperAndCriticalLocksReachMHPNodes) {
  const char *source = R"(
    @lock = global i8 0
    @crit = global [8 x i32] zeroinitializer

    declare void @fake_unique_lockC1E(i8*, i8*)
    declare void @fake_unique_lockD1Ev(i8*)
    declare void @__kmpc_critical(i8*, i32, [8 x i32]*)
    declare void @__kmpc_end_critical(i8*, i32, [8 x i32]*)

    define i32 @main() {
    entry:
      %wrapper = alloca i8
      call void @fake_unique_lockC1E(i8* %wrapper, i8* @lock)
      call void @fake_unique_lockD1Ev(i8* %wrapper)
      call void @__kmpc_critical(i8* null, i32 0, [8 x i32]* @crit)
      call void @__kmpc_end_critical(i8* null, i32 0, [8 x i32]* @crit)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);
  std::vector<const Instruction *> calls;
  for (const Instruction &inst : main_func->getEntryBlock()) {
    if (isa<CallBase>(&inst)) {
      calls.push_back(&inst);
    }
  }
  ASSERT_EQ(calls.size(), 4u);
  const Instruction *ctor = calls[0];
  const Instruction *dtor = calls[1];
  const Instruction *critical = calls[2];
  const Instruction *end_critical = calls[3];

  const ThreadFlowGraph &tfg = mhp.getThreadFlowGraph();
  auto ctor_nodes = tfg.getNodes(ctor);
  auto dtor_nodes = tfg.getNodes(dtor);
  auto critical_nodes = tfg.getNodes(critical);
  auto end_critical_nodes = tfg.getNodes(end_critical);
  ASSERT_FALSE(ctor_nodes.empty());
  ASSERT_FALSE(dtor_nodes.empty());
  ASSERT_FALSE(critical_nodes.empty());
  ASSERT_FALSE(end_critical_nodes.empty());

  EXPECT_EQ(ctor_nodes.front()->getLockValue(),
            dtor_nodes.front()->getLockValue());
  EXPECT_EQ(critical_nodes.front()->getLockValue(),
            module->getNamedGlobal("crit"));
  EXPECT_EQ(end_critical_nodes.front()->getLockValue(),
            module->getNamedGlobal("crit"));
}
TEST_F(MHPAnalysisTest, WrapperAcquireUsesUnderlyingMutexForHBHandoff) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_mutex_lock(i8*)
    declare i32 @pthread_mutex_unlock(i8*)
    declare void @fake_unique_lockC1E(i8*, i8*)
    declare void @fake_unique_lockD1Ev(i8*)

    @lock = global i8 0
    @shared = global i32 0

    define i8* @worker(i8* %arg) {
    entry:
      %wrapper = alloca i8
      call void @fake_unique_lockC1E(i8* %wrapper, i8* @lock)
      store i32 1, i32* @shared, align 4
      call void @fake_unique_lockD1Ev(i8* %wrapper)
      ret i8* null
    }

    define i32 @main() {
    entry:
      %tid = alloca i8
      call i32 @pthread_mutex_lock(i8* @lock)
      call i32 @pthread_create(i8* %tid, i8* null, i8* (i8*)* @worker, i8* null)
      call i32 @pthread_mutex_unlock(i8* @lock)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_locks, 1u);
  EXPECT_GE(stats.num_unlocks, 1u);
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *main_func = module->getFunction("main");
  const Function *worker_func = module->getFunction("worker");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(worker_func, nullptr);

  auto it = main_func->getEntryBlock().begin();
  ++it;
  ++it;
  ++it;
  const Instruction *unlock = &*it;
  const Instruction *worker_store = nullptr;
  for (const Instruction &inst : worker_func->getEntryBlock()) {
    if (isa<StoreInst>(&inst)) {
      worker_store = &inst;
      break;
    }
  }
  ASSERT_NE(worker_store, nullptr);

  EXPECT_TRUE(hb.mustPrecede(unlock, worker_store));
}
TEST_F(MHPAnalysisTest, LatchArriveAndWaitCreatesBarrierWaitNode) {
  const char *source = R"(
    declare void @fake_latch_arrive_and_wait(i8*)

    @latch = global i8 0

    define i32 @main() {
    entry:
      call void @fake_latch_arrive_and_wait(i8* @latch)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();

  const Function *main_func = module->getFunction("main");
  ASSERT_NE(main_func, nullptr);

  const Instruction *call = nullptr;
  for (const Instruction &inst : main_func->getEntryBlock()) {
    if (isa<CallBase>(&inst)) {
      call = &inst;
      break;
    }
  }
  ASSERT_NE(call, nullptr);

  const ThreadFlowGraph &tfg = mhp.getThreadFlowGraph();
  auto nodes = tfg.getNodes(call);
  ASSERT_EQ(nodes.size(), 1u);
  EXPECT_EQ(nodes.front()->getType(), SyncNodeType::BARRIER_WAIT);
}
TEST_F(MHPAnalysisTest, OpenMPTaskBodyMustPrecedeTaskwaitContinuation) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_omp_taskwait(i8*, i32)

    define i8* @task_body(i8* %arg) {
    entry:
      store i32 42, i32* @shared, align 4
      ret i8* null
    }

    define i32 @main() {
    entry:
      %task = alloca i8* (i8*)*, align 8
      store i8* (i8*)* @task_body, i8* (i8*)** %task, align 8
      %task_raw = bitcast i8* (i8*)** %task to i8*
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* %task_raw)
      call i32 @__kmpc_omp_taskwait(i8* null, i32 0)
      %after = load i32, i32* @shared, align 4
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *task_body = module->getFunction("task_body");
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(task_body, nullptr);
  ASSERT_NE(main_func, nullptr);

  const Instruction *task_store = &task_body->getEntryBlock().front();
  const Instruction *after = findInstructionByName(*main_func, "after");
  ASSERT_NE(task_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_TRUE(hb.mustPrecede(task_store, after));
  EXPECT_FALSE(mhp.mayHappenInParallel(task_store, after));
}
TEST_F(MHPAnalysisTest, JoinStatistics) {
  const char *source = R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
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

  auto stats = mhp.getStatistics();
  EXPECT_GE(stats.num_forks, 1);
  EXPECT_GE(stats.num_joins, 1);
}
TEST_F(MHPAnalysisTest, CUDADeviceSynchronizeOrdersKernelCompletion) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaDeviceSynchronize()

    define void @kernel() {
    entry:
      store i32 1, i32* @shared, align 4
      ret void
    }

    define i32 @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      %sync = call i32 @cudaDeviceSynchronize()
      %after = load i32, i32* @shared, align 4
      ret i32 %after
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Function *kernel = module->getFunction("kernel");
  const Function *main_func = module->getFunction("main");
  ASSERT_NE(kernel, nullptr);
  ASSERT_NE(main_func, nullptr);

  const Instruction *kernel_store = &kernel->getEntryBlock().front();
  const Instruction *after = findInstructionByName(*main_func, "after");
  ASSERT_NE(kernel_store, nullptr);
  ASSERT_NE(after, nullptr);

  EXPECT_FALSE(mhp.mayHappenInParallel(kernel_store, after));
  EXPECT_TRUE(hb.mustPrecede(kernel_store, after));
}
TEST_F(MHPAnalysisTest, RecursiveCallGraphDoesNotExplodeContexts) {
  const char *source = R"(
    define void @recur(i32 %n) {
    entry:
      %stop = icmp eq i32 %n, 0
      br i1 %stop, label %exit, label %step

    step:
      %next = sub i32 %n, 1
      call void @recur(i32 %next)
      br label %exit

    exit:
      ret void
    }

    define i32 @main() {
    entry:
      call void @recur(i32 2)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  EXPECT_NO_THROW(mhp.analyze());
}

TEST_F(MHPAnalysisTest, RecursiveCallDoesNotWireOuterInvocationContext) {
  const char *source = R"(
    define void @recur(i32 %n) {
    entry:
      %a = add i32 %n, 1
      %stop = icmp eq i32 %n, 0
      br i1 %stop, label %exit, label %step
    step:
      %next = sub i32 %n, 1
      call void @recur(i32 %next)
      br label %exit
    exit:
      ret void
    }
    define i32 @main() {
    entry:
      %dummy = add i32 1, 2
      call void @recur(i32 1)
      ret i32 0
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  MHPAnalysis mhp(*module);
  mhp.analyze();
  const Function *recur = module->getFunction("recur");
  ASSERT_NE(recur, nullptr);
  const Instruction *entry = findInstructionByName(*recur, "a");
  const Instruction *recursive = nullptr;
  for (const Instruction &inst : instructions(recur)) {
    const auto *call = dyn_cast<CallBase>(&inst);
    if (call && call->getCalledFunction() == recur) {
      recursive = &inst;
      break;
    }
  }
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(recursive, nullptr);
  const ThreadFlowGraph &tfg = mhp.getThreadFlowGraph();
  SyncNode *recursive_node = tfg.getNode(recursive);
  SyncNode *outer_entry = tfg.getNode(entry);
  ASSERT_NE(recursive_node, nullptr);
  ASSERT_NE(outer_entry, nullptr);
  EXPECT_FALSE(tfg.hasEdgeKind(recursive_node, outer_entry, EdgeKind::Call));
}

TEST_F(MHPAnalysisTest, FirstInstructionCallDoesNotReuseRootContext) {
  const char *source = R"(
    define void @helper() {
    entry:
      %h = add i32 1, 2
      ret void
    }
    define i32 @main() {
    entry:
      call void @helper()
      %post = add i32 3, 4
      ret i32 %post
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  MHPAnalysis mhp(*module);
  mhp.analyze();
  const Function *main_func = module->getFunction("main");
  const Function *helper = module->getFunction("helper");
  ASSERT_NE(main_func, nullptr);
  ASSERT_NE(helper, nullptr);
  const Instruction *call = &main_func->getEntryBlock().front();
  const Instruction *helper_inst = findInstructionByName(*helper, "h");
  ASSERT_NE(helper_inst, nullptr);
  const ThreadFlowGraph &tfg = mhp.getThreadFlowGraph();
  SyncNode *call_node = tfg.getNode(call, 0, 0);
  ASSERT_NE(call_node, nullptr);
  EXPECT_EQ(tfg.getThreadEntryNode(0), call_node);
  std::vector<SyncNode *> helper_nodes = tfg.getNodes(helper_inst, 0);
  ASSERT_EQ(helper_nodes.size(), 1u);
  EXPECT_NE(helper_nodes.front()->getCallContextID(), 0u);
  EXPECT_EQ(helper_nodes.front()->getCallContextID(), call_node->getNodeID());
}

TEST_F(MHPAnalysisTest, NullQueriesDoNotProduceMustSequentialFacts) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %x = add i32 1, 2
      ret i32 %x
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  MHPAnalysis mhp(*module);
  mhp.analyze();
  const Instruction *x =
      findInstructionByName(*module->getFunction("main"), "x");
  ASSERT_NE(x, nullptr);
  EXPECT_TRUE(mhp.mayHappenInParallel(nullptr, x));
  EXPECT_TRUE(mhp.mayHappenInParallel(x, nullptr));
  EXPECT_FALSE(mhp.mustBeSequential(nullptr, x));
  EXPECT_FALSE(mhp.mustBeSequential(x, nullptr));
}
TEST_F(MHPAnalysisTest, DeepCallChainPreservesNestedForkJoin) {
  const char *source = R"(
    @shared = global i32 0, align 4

    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8*)

    define i8* @worker(i8* %arg) {
    entry:
      %work = load i32, i32* @shared, align 4
      ret i8* null
    }

    define void @level4() {
    entry:
      %tid = alloca i8
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @worker, i8* null)
      call i32 @pthread_join(i8* %tid, i8* null)
      %after = load i32, i32* @shared, align 4
      ret void
    }

    define void @level3() { entry: call void @level4() ret void }
    define void @level2() { entry: call void @level3() ret void }
    define void @level1() { entry: call void @level2() ret void }
    define i32 @main() { entry: call void @level1() ret i32 0 }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MHPAnalysis mhp(*module);
  mhp.analyze();
  HappensBeforeAnalysis hb(*module, mhp);
  hb.analyze();

  const Instruction *work =
      findInstructionByName(*module->getFunction("worker"), "work");
  const Instruction *after =
      findInstructionByName(*module->getFunction("level4"), "after");
  ASSERT_NE(work, nullptr);
  ASSERT_NE(after, nullptr);
  EXPECT_FALSE(mhp.mayHappenInParallel(work, after));
  EXPECT_TRUE(hb.mustPrecede(work, after));
}
