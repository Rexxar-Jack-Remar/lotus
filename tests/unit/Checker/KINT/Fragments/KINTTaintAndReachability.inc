TEST_F(KINTCheckerTest, GetSinkFnsHandlesIndirectCalls) {
  const char *source = R"(
    define void @indirect(void (i32)* %fp, i32 %x) {
    entry:
      %v = add i32 %x, 1
      call void %fp(i32 %v)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("indirect");
  ASSERT_NE(F, nullptr);

  auto it = inst_begin(*F);
  ASSERT_NE(it, inst_end(*F));
  auto *add = dyn_cast<BinaryOperator>(&*it);
  ASSERT_NE(add, nullptr);

  auto sinks = kint::TaintAnalysis::get_sink_fns(add);
  EXPECT_TRUE(sinks.empty());
}

TEST_F(KINTCheckerTest, IsSinkReachableTerminatesOnCycles) {
  const char *source = R"(
    @g = global i32 0

    define void @cycle(i32 %x) {
    entry:
      store i32 %x, i32* @g
      %v = load i32, i32* @g
      %inc = add i32 %v, 1
      store i32 %inc, i32* @g
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  Function *F = module->getFunction("cycle");
  ASSERT_NE(F, nullptr);

  StoreInst *first_store = nullptr;
  for (Instruction &I : instructions(*F)) {
    if (auto *store = dyn_cast<StoreInst>(&I)) {
      first_store = store;
      break;
    }
  }
  ASSERT_NE(first_store, nullptr);

  kint::TaintAnalysis analysis;
  SetVector<Function *> taint_funcs;
  EXPECT_FALSE(analysis.is_sink_reachable(first_store, taint_funcs));
  EXPECT_TRUE(taint_funcs.empty());
}

TEST_F(KINTCheckerTest, PropagateTaintAcrossFunctionsFindsTransitiveTaint) {
  const char *source = R"(
    declare void @__mkint_sink0(i32)

    define void @mid2(i32 %x) {
    entry:
      %v2 = add i32 %x, 1
      call void @__mkint_sink0(i32 %v2)
      ret void
    }

    define void @mid1(i32 %x) {
    entry:
      %v1 = add i32 %x, 1
      call void @mid2(i32 %v1)
      ret void
    }

    define void @__mkint_ann_source(i32 %x) {
    entry:
      call void @mid1(i32 %x)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  kint::TaintAnalysis analysis;
  MapVector<Function *, std::vector<CallInst *>> func2tsrc;
  SetVector<Function *> taint_funcs;
  SetVector<StringRef> callback_tsrc_fn;

  for (Function &F : *module) {
    auto taint_sources = analysis.get_taint_source(F);
    analysis.mark_func_sinks(F, callback_tsrc_fn);
    if (kint::TaintAnalysis::is_taint_src(F.getName())) {
      func2tsrc[&F] = std::move(taint_sources);
    }
  }

  analysis.propagate_taint_across_functions(*module, func2tsrc, taint_funcs);

  Function *mid1 = module->getFunction("mid1");
  Function *mid2 = module->getFunction("mid2");
  ASSERT_NE(mid1, nullptr);
  ASSERT_NE(mid2, nullptr);
  EXPECT_TRUE(taint_funcs.contains(mid1));
  EXPECT_TRUE(taint_funcs.contains(mid2));
}

TEST_F(KINTCheckerTest, ArgumentSinkUseIsRecognized) {
  const char *source = R"(
    declare i8* @malloc(i64)

    define void @__mkint_ann_alloc_user(i64 %n) {
    entry:
      %p = call i8* @malloc(i64 %n)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  kint::TaintAnalysis analysis;
  MapVector<Function *, std::vector<CallInst *>> func2tsrc;
  SetVector<Function *> taint_funcs;
  SetVector<StringRef> callback_tsrc_fn;

  for (Function &F : *module) {
    auto taint_sources = analysis.get_taint_source(F);
    analysis.mark_func_sinks(F, callback_tsrc_fn);
    if (kint::TaintAnalysis::is_taint_src(F.getName())) {
      func2tsrc[&F] = std::move(taint_sources);
    }
  }

  analysis.propagate_taint_across_functions(*module, func2tsrc, taint_funcs);

  Function *malloc_fn = module->getFunction("malloc");
  ASSERT_NE(malloc_fn, nullptr);
  EXPECT_TRUE(taint_funcs.contains(malloc_fn));
}
