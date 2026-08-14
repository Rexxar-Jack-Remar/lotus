TEST_F(CUDAAnalysisTest, TracksUnifiedMemoryMetadata) {
  const char *source = R"(
    declare i32 @cudaMallocManaged(i8**, i64, i32)
    declare i32 @cudaMallocHost(i8**, i64)
    declare i32 @cudaMemPrefetchAsync(i8*, i64, i32, i8*)

    define i32 @main() {
    entry:
      %managed = alloca i8*
      %host = alloca i8*
      %m = call i32 @cudaMallocManaged(i8** %managed, i64 64, i32 1)
      %managed_ptr = load i8*, i8** %managed
      %p = call i32 @cudaMemPrefetchAsync(i8* %managed_ptr, i64 64, i32 2, i8* null)
      %h = call i32 @cudaMallocHost(i8** %host, i64 32)
      %sum = add i32 %m, %p
      %sum2 = add i32 %sum, %h
      ret i32 %sum2
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getUnifiedMemory().size(), 3u);

  size_t managed_count = 0;
  size_t prefetch_count = 0;
  size_t host_count = 0;
  for (const auto &info : analysis.getUnifiedMemory()) {
    if (info.is_prefetch) {
      ++prefetch_count;
      EXPECT_EQ(info.size, 64u);
      EXPECT_EQ(info.device_id, 2);
    } else if (info.is_managed) {
      ++managed_count;
      EXPECT_EQ(info.size, 64u);
      EXPECT_EQ(info.device_id, -1);
    } else {
      ++host_count;
      EXPECT_EQ(info.size, 32u);
    }
  }

  EXPECT_EQ(managed_count, 1u);
  EXPECT_EQ(prefetch_count, 1u);
  EXPECT_EQ(host_count, 1u);
}
TEST_F(CUDAAnalysisTest, ClassifiesUnifiedMemoryAttachAdviseAndHostAlloc) {
  const char *source = R"(
    %stream_t = type opaque

    declare i32 @cudaMallocManaged(i8**, i64, i32)
    declare i32 @cudaStreamAttachMemAsync(%stream_t*, i8*, i64, i32)
    declare i32 @cudaMemAdvise(i8*, i64, i32, i32)
    declare i32 @cudaHostAlloc(i8**, i64, i32)

    define i32 @main(%stream_t* %stream) {
    entry:
      %managed = alloca i8*
      %host = alloca i8*
      %m = call i32 @cudaMallocManaged(i8** %managed, i64 128, i32 1)
      %managed_ptr = load i8*, i8** %managed
      %attach = call i32 @cudaStreamAttachMemAsync(%stream_t* %stream,
                                                   i8* %managed_ptr, i64 0,
                                                   i32 2)
      %advise = call i32 @cudaMemAdvise(i8* %managed_ptr, i64 128, i32 1,
                                        i32 7)
      %host_alloc = call i32 @cudaHostAlloc(i8** %host, i64 64, i32 0)
      %sum = add i32 %m, %attach
      %sum2 = add i32 %sum, %advise
      %sum3 = add i32 %sum2, %host_alloc
      ret i32 %sum3
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getUnifiedMemory().size(), 4u);

  size_t managed_count = 0;
  size_t attach_count = 0;
  size_t advise_count = 0;
  size_t host_count = 0;
  for (const auto &info : analysis.getUnifiedMemory()) {
    if (info.is_attach) {
      ++attach_count;
      EXPECT_NE(info.ptr, nullptr);
      EXPECT_EQ(info.stream, module->getFunction("main")->getArg(0));
      EXPECT_EQ(info.size, 0u);
    } else if (info.is_advise) {
      ++advise_count;
      EXPECT_EQ(info.size, 128u);
      EXPECT_EQ(info.device_id, -1);
    } else if (info.is_managed) {
      ++managed_count;
      EXPECT_EQ(info.size, 128u);
    } else {
      ++host_count;
      EXPECT_EQ(info.size, 64u);
    }
  }

  EXPECT_EQ(managed_count, 1u);
  EXPECT_EQ(attach_count, 1u);
  EXPECT_EQ(advise_count, 1u);
  EXPECT_EQ(host_count, 1u);
}
TEST_F(CUDAAnalysisTest, ReportsStructuredModelGapsForManagedAndUnknownUnifiedPointers) {
  const char *source = R"(
    declare i32 @cudaMallocManaged(i8**, i64, i32)
    declare i32 @cudaMemAdvise(i8*, i64, i32, i32)

    define i32 @main(i8* %unknown_ptr) {
    entry:
      %managed = alloca i8*
      %m = call i32 @cudaMallocManaged(i8** %managed, i64 64, i32 1)
      %managed_ptr = load i8*, i8** %managed
      %a0 = call i32 @cudaMemAdvise(i8* %unknown_ptr, i64 64, i32 0, i32 0)
      %a1 = call i32 @cudaMemAdvise(i8* %managed_ptr, i64 64, i32 1, i32 0)
      %sum = add i32 %m, %a0
      %sum2 = add i32 %sum, %a1
      ret i32 %sum2
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &gaps = analysis.getAbstractState().getModelGaps();
  EXPECT_GE(gaps.size(), 1u);

  bool saw_unknown_ptr_gap = false;
  for (const auto &gap : gaps) {
    EXPECT_FALSE(gap.related_instructions.empty());
    EXPECT_GT(gap.confidence, 0.0);
    EXPECT_NE(gap.explanation.find("unknown memory-space classification"),
              std::string::npos);
    saw_unknown_ptr_gap = true;
  }

  EXPECT_TRUE(saw_unknown_ptr_gap);
}
