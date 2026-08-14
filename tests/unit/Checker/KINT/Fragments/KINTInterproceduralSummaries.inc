TEST_F(KINTCheckerTest, BugDetectionKeepsDistinctBugTypesPerInstruction) {
  const char *source = R"(
    define i32 @div(i32 %x, i32 %y) {
    entry:
      %q = sdiv i32 %x, %y
      ret i32 %q
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("div");
  ASSERT_NE(F, nullptr);

  auto *div = dyn_cast<BinaryOperator>(&*inst_begin(*F));
  ASSERT_NE(div, nullptr);

  kint::BugDetection bug_detection;
  bug_detection.recordBug(div, kint::interr::INT_OVERFLOW);
  bug_detection.recordBug(div, kint::interr::DIV_BY_ZERO);

  EXPECT_EQ(bug_detection.getBugPaths().size(), 2u);
}

TEST_F(KINTCheckerTest, InterprocSummaryPreservesPtrToIntIntToPtrRoundTrip) {
  const char *source = R"(
    define i32* @roundtrip(i32* %p) {
    entry:
      %i = ptrtoint i32* %p to i64
      %q = inttoptr i64 %i to i32*
      ret i32* %q
    }

    define i32 @main() {
    entry:
      %x = alloca i32
      store i32 1, i32* %x
      %p = call i32* @roundtrip(i32* %x)
      %v = load i32, i32* %p
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;
  kint::InterprocSummaryMode = kint::SummaryMode::On;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*main, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, FieldSensitiveCallHavocPreservesSiblingStructField) {
  const char *source = R"(
    declare void @touch(i32*)

    define i32 @main() {
    entry:
      %s = alloca { i32, i32 }
      %f0 = getelementptr inbounds { i32, i32 }, { i32, i32 }* %s, i64 0,
                                      i32 0
      %f1 = getelementptr inbounds { i32, i32 }, { i32, i32 }* %s, i64 0,
                                      i32 1
      store i32 1, i32* %f0
      store i32 1, i32* %f1
      call void @touch(i32* %f0)
      %v = load i32, i32* %f1
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;
  kint::InterprocSummaryMode = kint::SummaryMode::Off;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*main, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, AtomicRMWPreservesSiblingStructField) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %s = alloca { i32, i32 }
      %f0 = getelementptr inbounds { i32, i32 }, { i32, i32 }* %s, i64 0,
                                      i32 0
      %f1 = getelementptr inbounds { i32, i32 }, { i32, i32 }* %s, i64 0,
                                      i32 1
      store i32 1, i32* %f0
      store i32 1, i32* %f1
      %old = atomicrmw add i32* %f0, i32 1 seq_cst
      %v = load i32, i32* %f1
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;
  kint::InterprocSummaryMode = kint::SummaryMode::Off;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*main, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest,
       InterprocSummaryPreservesReturnedPointerOffsetThroughIntCast) {
  const char *source = R"(
    define i32* @shift1(i32* %p) {
    entry:
      %q = getelementptr inbounds i32, i32* %p, i64 1
      ret i32* %q
    }

    define i32 @main() {
    entry:
      %arr = alloca [2 x i32]
      %p0 = getelementptr inbounds [2 x i32], [2 x i32]* %arr, i64 0, i64 0
      store i32 1, i32* %p0
      %q = call i32* @shift1(i32* %p0)
      %i = ptrtoint i32* %q to i64
      %r = inttoptr i64 %i to i32*
      store i32 0, i32* %r
      %v = load i32, i32* %p0
      %d = sdiv i32 42, %v
      ret i32 %d
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;
  kint::InterprocSummaryMode = kint::SummaryMode::On;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*main, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, WideConstantPreservesHighBits) {
  const char *source = R"(
    define i128 @wide() {
    entry:
      ret i128 18446744073709551616
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  Function *F = module->getFunction("wide");
  ASSERT_NE(F, nullptr);
  auto *ret = dyn_cast<ReturnInst>(F->getEntryBlock().getTerminator());
  ASSERT_NE(ret, nullptr);
  auto *ci = dyn_cast<ConstantInt>(ret->getReturnValue());
  ASSERT_NE(ci, nullptr);

  z3::context ctx;
  z3::solver solver(ctx);
  DenseMap<const Value *, std::optional<z3::expr>> empty;
  kint::BugDetection bug_detection;
  z3::expr actual = bug_detection.v2sym(ci, empty, solver);
  z3::expr expected = bvValFromAPInt(ctx, ci->getValue());

  solver.add(actual != expected);
  EXPECT_EQ(solver.check(), z3::unsat);
}

TEST_F(KINTCheckerTest,
       InterprocSummarySuppressesFalseDivZeroThroughPointerArg) {
  const char *source = R"(
    define void @set_one(i32* %p) {
    entry:
      store i32 1, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i32
      store i32 0, i32* %x
      call void @set_one(i32* %x)
      %v = load i32, i32* %x
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummarySuppressesFalseDivZeroThroughGlobal) {
  const char *source = R"(
    @g = global i32 0

    define void @setg() {
    entry:
      store i32 1, i32* @g
      ret void
    }

    define i32 @main() {
    entry:
      call void @setg()
      %v = load i32, i32* @g
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummaryFramesUnmodifiedPointerArgument) {
  const char *source = R"(
    define void @touch_first(i32* %p, i32* %q) {
    entry:
      store i32 1, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i32
      %y = alloca i32
      store i32 0, i32* %x
      store i32 1, i32* %y
      call void @touch_first(i32* %x, i32* %y)
      %v = load i32, i32* %y
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;
  kint::InterprocSummaryMode = kint::SummaryMode::On;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*main, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest,
       InterprocSummaryPreservesSiblingFieldThroughPointerArgOffset) {
  const char *source = R"(
    define void @touch(i32* %p) {
    entry:
      store i32 1, i32* %p
      ret void
    }

    define i32 @main() {
    entry:
      %s = alloca { i32, i32 }
      %f0 = getelementptr inbounds { i32, i32 }, { i32, i32 }* %s, i64 0,
                                      i32 0
      %f1 = getelementptr inbounds { i32, i32 }, { i32, i32 }* %s, i64 0,
                                      i32 1
      store i32 0, i32* %f0
      store i32 1, i32* %f1
      call void @touch(i32* %f1)
      %v = load i32, i32* %f0
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;
  kint::InterprocSummaryMode = kint::SummaryMode::On;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*main, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummaryPropagatesNestedPointerModification) {
  const char *source = R"(
    define void @leaf(i32* %p) {
    entry:
      store i32 1, i32* %p
      ret void
    }

    define void @mid(i32* %p) {
    entry:
      call void @leaf(i32* %p)
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i32
      store i32 0, i32* %x
      call void @mid(i32* %x)
      %v = load i32, i32* %x
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummarySupportsPointerReturnFromArgument) {
  const char *source = R"(
    define i32* @idptr(i32* %p) {
    entry:
      ret i32* %p
    }

    define i32 @main() {
    entry:
      %x = alloca i32
      store i32 1, i32* %x
      %p = call i32* @idptr(i32* %x)
      %v = load i32, i32* %p
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummarySupportsPointerReturnFromGlobal) {
  const char *source = R"(
    @g = global i32 0

    define i32* @getg() {
    entry:
      ret i32* @g
    }

    define i32 @main() {
    entry:
      store i32 1, i32* @g
      %p = call i32* @getg()
      %v = load i32, i32* %p
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest,
       InterprocSummarySupportsEscapedAllocatorInitialization) {
  const char *source = R"(
    declare i32* @malloc(i64)

    define i32* @alloc_init() {
    entry:
      %p = call i32* @malloc(i64 4)
      store i32 1, i32* %p
      ret i32* %p
    }

    define i32 @main() {
    entry:
      %p = call i32* @alloc_init()
      %v = load i32, i32* %p
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest,
       InterprocSummaryFramesCallerHeapAcrossEscapedAllocator) {
  const char *source = R"(
    declare i32* @malloc(i64)

    define i32* @alloc_init() {
    entry:
      %p = call i32* @malloc(i64 4)
      store i32 1, i32* %p
      ret i32* %p
    }

    define i32 @main() {
    entry:
      %q = call i32* @malloc(i64 4)
      store i32 1, i32* %q
      %p = call i32* @alloc_init()
      %v = load i32, i32* %q
      %d = sdiv i32 42, %v
      ret i32 %d
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest,
       InterprocSummaryExistentiallySeparatesHiddenPathWitnessesPerCall) {
  const char *source = R"(
    declare i32 @nondet() readnone

    define i32 @choose() {
    entry:
      %x = call i32 @nondet()
      %cmp = icmp eq i32 %x, 0
      br i1 %cmp, label %zero, label %one

    zero:
      ret i32 0

    one:
      ret i32 1
    }

    define i32 @main() {
    entry:
      %a = call i32 @choose()
      %b = call i32 @choose()
      %sum = add i32 %a, %b
      %is_one = icmp eq i32 %sum, 1
      br i1 %is_one, label %bug, label %safe

    bug:
      %q = sdiv i32 42, 0
      ret i32 %q

    safe:
      ret i32 0
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;
  kint::InterprocSummaryMode = kint::SummaryMode::On;

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  runPass(*module);
  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*main, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummaryPrunesImpossibleCallerBranch) {
  const char *source = R"(
    define i32 @ret_zero() {
    entry:
      ret i32 0
    }

    define i32 @main() {
    entry:
      %v = call i32 @ret_zero()
      %is_one = icmp eq i32 %v, 1
      br i1 %is_one, label %bug, label %safe

    bug:
      %q = sdiv i32 42, 0
      ret i32 %q

    safe:
      ret i32 0
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummaryHandlesPhiMergedReturnCases) {
  const char *source = R"(
    declare i32 @nondet() readnone

    define i32 @choose_phi() {
    entry:
      %x = call i32 @nondet()
      %cmp = icmp eq i32 %x, 0
      br i1 %cmp, label %zero, label %two

    zero:
      br label %merge

    two:
      br label %merge

    merge:
      %v = phi i32 [ 0, %zero ], [ 2, %two ]
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %v = call i32 @choose_phi()
      %is_one = icmp eq i32 %v, 1
      br i1 %is_one, label %bug, label %safe

    bug:
      %q = sdiv i32 42, 0
      ret i32 %q

    safe:
      ret i32 0
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummaryTracksBooleanComposedPathCondition) {
  const char *source = R"(
    define i32 @range_flag(i32 %x) {
    entry:
      %gt_zero = icmp sgt i32 %x, 0
      %lt_ten = icmp slt i32 %x, 10
      %in_range = and i1 %gt_zero, %lt_ten
      br i1 %in_range, label %safe, label %bug

    safe:
      ret i32 1

    bug:
      ret i32 0
    }

    define i32 @main() {
    entry:
      %v = call i32 @range_flag(i32 5)
      %is_zero = icmp eq i32 %v, 0
      br i1 %is_zero, label %bug, label %safe

    bug:
      %q = sdiv i32 42, 0
      ret i32 %q

    safe:
      ret i32 0
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_FALSE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

TEST_F(KINTCheckerTest, InterprocSummaryFallsBackOnAliasedActuals) {
  const char *source = R"(
    define void @write_both(i32* %p, i32* %q) {
    entry:
      store i32 1, i32* %p, align 4
      store i32 0, i32* %q, align 4
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i32, align 4
      store i32 7, i32* %x, align 4
      call void @write_both(i32* %x, i32* %x)
      %v = load i32, i32* %x, align 4
      %q = sdiv i32 42, %v
      ret i32 %q
    }
  )";

  const auto oldCheckDivByZero = kint::CheckDivByZero;
  const auto oldAnalyzeAllFunctions = kint::AnalyzeAllFunctions.getValue();
  const auto oldSummaryMode = kint::InterprocSummaryMode.getValue();

  kint::CheckDivByZero = true;
  kint::AnalyzeAllFunctions = true;

  auto withoutSummary = parseModule(source);
  ASSERT_NE(withoutSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::Off;
  runPass(*withoutSummary);
  Function *mainWithout = withoutSummary->getFunction("main");
  ASSERT_NE(mainWithout, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWithout, Instruction::SDiv));

  auto withSummary = parseModule(source);
  ASSERT_NE(withSummary, nullptr);
  kint::InterprocSummaryMode = kint::SummaryMode::On;
  runPass(*withSummary);
  Function *mainWith = withSummary->getFunction("main");
  ASSERT_NE(mainWith, nullptr);
  EXPECT_TRUE(hasKintErrorMetadata(*mainWith, Instruction::SDiv));

  kint::CheckDivByZero = oldCheckDivByZero;
  kint::AnalyzeAllFunctions = oldAnalyzeAllFunctions;
  kint::InterprocSummaryMode = oldSummaryMode;
}

} // namespace
