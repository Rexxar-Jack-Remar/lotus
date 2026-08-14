TEST(NPA, InterproceduralTaintReachabilityTracksCalleePointerStores) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)

    define void @store_ptr(i8** %slot, i8* %p) {
    entry:
      store i8* %p, i8** %slot, align 8
      ret void
    }

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %slot = alloca i8*, align 8
      store i8* null, i8** %slot, align 8
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      call void @store_ptr(i8** %slot, i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE fgets Arg0 D T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *slot = findInstructionByName(*mainFn, "slot");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(slot, nullptr);
  EXPECT_TRUE(result.isReachableMemoryTainted(after, slot));
}

TEST(NPA, InterproceduralTaintReachabilityTracksCalleeGlobalPointerStores) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @gp = global i8* null
    declare i8* @fgets(i8*, i64, i8*)

    define void @set_global_ptr(i8* %p) {
    entry:
      store i8* %p, i8** @gp, align 8
      ret void
    }

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      call void @set_global_ptr(i8* %p0)
      br label %after

    after:
      %q = load i8*, i8** @gp, align 8
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE fgets Arg0 D T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *gp = module->getGlobalVariable("gp");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(gp, nullptr);
  EXPECT_TRUE(result.isReachableMemoryTainted(after, gp));
}

TEST(NPA, InterproceduralTaintReachabilityTracksCalleePointerStoreClears) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)

    define void @clear_slot(i8** %slot) {
    entry:
      store i8* null, i8** %slot, align 8
      ret void
    }

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %slot = alloca i8*, align 8
      store i8* %p0, i8** %slot, align 8
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      call void @clear_slot(i8** %slot)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE fgets Arg0 D T\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *slot = findInstructionByName(*mainFn, "slot");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(slot, nullptr);
  EXPECT_FALSE(result.isReachableMemoryTainted(after, slot));
}

TEST(NPA, InterproceduralTaintReachableDerefPipePropagatesTransitively) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)
    declare i8* @memcpy(i8*, i8*, i64)

    define i32 @main() {
    entry:
      %srcbuf = alloca [8 x i8], align 1
      %src = getelementptr inbounds [8 x i8], [8 x i8]* %srcbuf, i64 0, i64 0
      %srcslot = alloca i8*, align 8
      store i8* %src, i8** %srcslot, align 8

      %dstbuf = alloca [8 x i8], align 1
      %dst = getelementptr inbounds [8 x i8], [8 x i8]* %dstbuf, i64 0, i64 0
      %dstslot = alloca i8*, align 8
      store i8* %dst, i8** %dstslot, align 8

      %call = call i8* @fgets(i8* %src, i64 8, i8* null)
      %srcslot.raw = bitcast i8** %srcslot to i8*
      %dstslot.raw = bitcast i8** %dstslot to i8*
      %copy = call i8* @memcpy(i8* %dstslot.raw, i8* %srcslot.raw, i64 8)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE fgets Ret V T\n"
                           "SOURCE fgets Arg0 D T\n"
                           "PIPE fgets Ret V Arg0 V\n"
                           "PIPE memcpy Arg0 R Arg1 R\n"
                           "PIPE memcpy Ret V Arg0 V\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *src = findInstructionByName(*mainFn, "src");
  const auto *srcslotRaw = findInstructionByName(*mainFn, "srcslot.raw");
  const auto *dst = findInstructionByName(*mainFn, "dst");
  const auto *dstslotRaw = findInstructionByName(*mainFn, "dstslot.raw");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(src, nullptr);
  ASSERT_NE(srcslotRaw, nullptr);
  ASSERT_NE(dst, nullptr);
  ASSERT_NE(dstslotRaw, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, dst));
  EXPECT_TRUE(result.isReachableMemoryTainted(after, dstslotRaw));
}

TEST(NPA, InterproceduralTaintAfterArgPipeUsesShippedSpecOrder) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @fmt = private unnamed_addr constant [3 x i8] c"%s\00", align 1
    declare i8* @fgets(i8*, i64, i8*)
    declare i32 @snprintf(i8*, i64, i8*, ...)

    define i32 @main() {
    entry:
      %srcbuf = alloca [8 x i8], align 1
      %src = getelementptr inbounds [8 x i8], [8 x i8]* %srcbuf, i64 0, i64 0
      %dstbuf = alloca [8 x i8], align 1
      %dst = getelementptr inbounds [8 x i8], [8 x i8]* %dstbuf, i64 0, i64 0
      %fmtptr = getelementptr inbounds [3 x i8], [3 x i8]* @fmt, i64 0, i64 0
      %in = call i8* @fgets(i8* %src, i64 8, i8* null)
      %n = call i32 (i8*, i64, i8*, ...) @snprintf(i8* %dst, i64 8,
                                                   i8* %fmtptr, i8* %src)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  const std::string configPath =
      writeTempTaintConfig("SOURCE fgets Arg0 D T\n"
                           "PIPE snprintf Arg0 D AfterArg2 D\n");
  ASSERT_FALSE(configPath.empty());

  npa::InterTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *src = findInstructionByName(*mainFn, "src");
  const auto *dst = findInstructionByName(*mainFn, "dst");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(src, nullptr);
  ASSERT_NE(dst, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, src));
  EXPECT_TRUE(result.isMemoryTainted(after, dst));
}

TEST(NPA, InterproceduralTaintResolvesIndirectExternalSourceViaAliasWrapper) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @srcfp = global i32 ()* @getchar
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %fp = load i32 ()*, i32 ()** @srcfp
      %x = call i32 %fp()
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  const auto *after = findBlock(*mainFn, "after");
  const auto *x = findInstructionByName(*mainFn, "x");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(x, nullptr);
  EXPECT_TRUE(result.isValueTainted(after, x));
}

TEST(NPA, InterproceduralTaintDetectsSinkHits) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)
    declare i32 @printf(i8*, ...)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %sink = call i32 (i8*, ...) @printf(i8* %p0)
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *sink =
      llvm::dyn_cast<llvm::CallBase>(findInstructionByName(*mainFn, "sink"));
  ASSERT_NE(sink, nullptr);
  EXPECT_TRUE(result.isSinkTriggered(sink));
  EXPECT_FALSE(result.sinkHits.empty());
}

TEST(NPA, InterproceduralTaintResolvesIndirectExternalSinkViaAliasWrapper) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @sinkfp = global i32 (i8*)* @system
    declare i8* @fgets(i8*, i64, i8*)
    declare i32 @system(i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %fp = load i32 (i8*)*, i32 (i8*)** @sinkfp
      %sink = call i32 %fp(i8* %p0)
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  const auto *sink =
      llvm::dyn_cast<llvm::CallBase>(findInstructionByName(*mainFn, "sink"));
  ASSERT_NE(sink, nullptr);
  EXPECT_TRUE(result.isSinkTriggered(sink));
}

TEST(NPA, InterproceduralTaintSinkReplayUsesAnalysisSpecificCalleeResolution) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @sinkfp = global i32 (i8*)* @system
    declare i8* @fgets(i8*, i64, i8*)
    declare i32 @system(i8*)

    define i32 @system_local(i8* %arg) {
    entry:
      ret i32 0
    }

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %fp = load i32 (i8*)*, i32 (i8*)** @sinkfp
      %sink = call i32 %fp(i8* %p0)
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *sink =
      llvm::dyn_cast<llvm::CallBase>(findInstructionByName(*mainFn, "sink"));
  ASSERT_NE(sink, nullptr);
  ASSERT_EQ(result.sinkHits.size(), 1u);
  EXPECT_TRUE(result.isSinkTriggered(sink));
}

TEST(NPA, InterproceduralTaintIgnoresUninitializedOnlySinkPaths) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @malloc(i64)
    declare i32 @printf(i8*, ...)

    define i32 @main() {
    entry:
      %p = call i8* @malloc(i64 4)
      %sink = call i32 (i8*, ...) @printf(i8* %p)
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *sink =
      llvm::dyn_cast<llvm::CallBase>(findInstructionByName(*mainFn, "sink"));
  ASSERT_NE(sink, nullptr);
  EXPECT_FALSE(result.isSinkTriggered(sink));
}

TEST(NPA, InterproceduralTaintObserverLibraryCallDoesNotKillArgumentTaint) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)
    declare i64 @strlen(i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %len = call i64 @strlen(i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, p0));
}

TEST(NPA, InterproceduralTaintFailsClosedWhenConfigIsMissing) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %x = call i32 @getchar()
      ret i32 %x
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterTaint::Options options;
  options.taint_config_path = "/definitely/missing/npa-taint.spec";
  auto result = npa::InterTaint::run(*module, wrapper, options);

  EXPECT_TRUE(result.status.configuration_error);
  EXPECT_FALSE(result.status.overall_converged);
  EXPECT_TRUE(result.sinkHits.empty());
}

TEST(NPA,
     InterproceduralTaintUnsupportedUninitializedSpecsDoNotAbortByDefault) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %x = call i32 @getchar()
      ret i32 %x
    }
  )");
  ASSERT_TRUE(module);

  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V U\n");
  ASSERT_FALSE(configPath.empty());

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  EXPECT_TRUE(result.status.unsupported_specs);
  EXPECT_TRUE(result.status.summary_solve.converged);
  EXPECT_FALSE(result.valueBits.empty());
  EXPECT_TRUE(result.status.approximated);
  EXPECT_FALSE(result.status.overall_converged);
}

TEST(NPA,
     InterproceduralTaintRejectsUnsupportedUninitializedSpecsInStrictMode) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %x = call i32 @getchar()
      ret i32 %x
    }
  )");
  ASSERT_TRUE(module);

  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V U\n");
  ASSERT_FALSE(configPath.empty());

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterTaint::Options options;
  options.taint_config_path = configPath;
  options.fail_on_unsupported_specs = true;
  auto result = npa::InterTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  EXPECT_TRUE(result.status.unsupported_specs);
  EXPECT_FALSE(result.status.summary_solve.converged);
  EXPECT_TRUE(result.valueBits.empty());
  EXPECT_FALSE(result.status.overall_converged);
}

TEST(NPA, InterproceduralTaintSupportsReturnMemorySourceSpecs) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @mkbuf()

    define i32 @main() {
    entry:
      %p = call i8* @mkbuf()
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  const std::string configPath = writeTempTaintConfig("SOURCE mkbuf Ret D T\n");
  ASSERT_FALSE(configPath.empty());

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *p = findInstructionByName(*mainFn, "p");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(result.isMemoryTainted(after, p));
}

TEST(
    NPA,
    InterproceduralTaintUsesAnalysisSpecificCalleeResolutionDuringPropagation) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @fp = global i32 (i8*)* @clean_ret
    declare i32 @getchar()

    define i32 @clean_ret(i8* %p) {
    entry:
      ret i32 0
    }

    define i32 @tainted_ret(i8* %p) {
    entry:
      %t = call i32 @getchar()
      ret i32 %t
    }

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %f = load i32 (i8*)*, i32 (i8*)** @fp
      %r = call i32 %f(i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterTaint::Options options;
  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());
  options.taint_config_path = configPath;
  auto result = npa::InterTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *r = findInstructionByName(*mainFn, "r");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(r, nullptr);
  EXPECT_FALSE(result.isValueTainted(after, r));
}

TEST(NPA, InterproceduralTaintSinkReplayAfterSelectTransformation) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @fgets(i8*, i64, i8*)
    declare i32 @system(i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %other = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %p1 = getelementptr inbounds [8 x i8], [8 x i8]* %other, i64 0, i64 0
      %src = call i8* @fgets(i8* %p0, i64 8, i8* null)
      %choice = select i1 true, i8* %p0, i8* %p1
      %sink = call i32 @system(i8* %choice)
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  const auto *sink =
      llvm::dyn_cast<llvm::CallBase>(findInstructionByName(*mainFn, "sink"));
  ASSERT_NE(sink, nullptr);
  EXPECT_TRUE(result.isSinkTriggered(sink));
}

TEST(NPA, InterproceduralTaintLoadDoesNotUsePointerValueTaintByDefault) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %slot = alloca i32, align 4
      store i32 7, i32* %slot, align 4
      %t = call i32 @getchar()
      %cmp = icmp eq i32 %t, 0
      %p = select i1 %cmp, i32* %slot, i32* %slot
      %x = load i32, i32* %p, align 4
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  const auto *after = findBlock(*mainFn, "after");
  const auto *x = findInstructionByName(*mainFn, "x");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(x, nullptr);
  EXPECT_FALSE(result.isValueTainted(after, x));
}

TEST(NPA, InterproceduralTaintUnknownExternalCallIsNoop) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @mystery(i8*)
    declare i32 @system(i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %r = call i8* @mystery(i8* %p0)
      %sink = call i32 @system(i8* %p0)
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  const auto *sink =
      llvm::dyn_cast<llvm::CallBase>(findInstructionByName(*mainFn, "sink"));
  ASSERT_NE(sink, nullptr);
  EXPECT_FALSE(result.isSinkTriggered(sink));
}

TEST(NPA, InterproceduralTaintUnknownExternalPointerArgLeavesFactsUntainted) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i8* @mystery(i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %r = call i8* @mystery(i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterTaint::run(*module, wrapper);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  EXPECT_FALSE(result.isMemoryTainted(after, p0));
}

TEST(NPA, InterproceduralTaintUnresolvedIndirectCallIsNoop) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @driver(i8* (i8*)* %fp) {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %r = call i8* %fp(i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterTaint::run(*module, wrapper);

  const auto *driverFn = module->getFunction("driver");
  ASSERT_NE(driverFn, nullptr);
  const auto *after = findBlock(*driverFn, "after");
  const auto *p = findInstructionByName(*driverFn, "p0");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p, nullptr);
  EXPECT_FALSE(result.isMemoryTainted(after, p));
}

TEST(NPA,
     InterproceduralTaintHandlesExternalPointerGlobalsWithoutInitializers) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @ext = external global i8*

    define i32 @main() {
    entry:
      %p = load i8*, i8** @ext, align 8
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  EXPECT_FALSE(result.status.configuration_error);
  EXPECT_TRUE(result.status.summary_solve.converged);
}

TEST(NPA, InterproceduralTaintHandlesCyclicConstantBackedIndirectTargets) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    @fp = global i8* bitcast (i8** @fp to i8*)

    define i32 @main() {
    entry:
      %buf = alloca [8 x i8], align 1
      %p0 = getelementptr inbounds [8 x i8], [8 x i8]* %buf, i64 0, i64 0
      %raw = load i8*, i8** @fp, align 8
      %callee = bitcast i8* %raw to i32 (i8*)*
      %r = call i32 %callee(i8* %p0)
      br label %after

    after:
      ret i32 0
    }
  )");
  ASSERT_TRUE(module);

  const std::string configPath =
      writeTempTaintConfig("SOURCE getchar Ret V T\n");
  ASSERT_FALSE(configPath.empty());

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  npa::InterTaint::Options options;
  options.taint_config_path = configPath;
  auto result = npa::InterTaint::run(*module, wrapper, options);
  llvm::sys::fs::remove(configPath);

  const auto *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);
  const auto *after = findBlock(*mainFn, "after");
  const auto *p0 = findInstructionByName(*mainFn, "p0");
  ASSERT_NE(after, nullptr);
  ASSERT_NE(p0, nullptr);
  EXPECT_FALSE(result.status.configuration_error);
  EXPECT_TRUE(result.status.summary_solve.converged);
  EXPECT_FALSE(result.isMemoryTainted(after, p0));
}

TEST(NPA, InterproceduralTaintTensorStrategyFallsBackToScc) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare i32 @getchar()

    define i32 @main() {
    entry:
      %x = call i32 @getchar()
      ret i32 %x
    }
  )");
  ASSERT_TRUE(module);

  lotus::AliasAnalysisWrapper wrapper(*module,
                                      lotus::AAConfig::SparrowAA_NoCtx());
  auto result = npa::InterTaint::run(
      *module, wrapper, false, npa::LinearStrategy::TensorProduct);

  EXPECT_TRUE(result.status.summary_solve.converged);
}
