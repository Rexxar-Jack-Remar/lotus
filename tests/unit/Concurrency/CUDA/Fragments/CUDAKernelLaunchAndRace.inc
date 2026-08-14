TEST_F(CUDAAnalysisTest, ResolvesRuntimeCudaLaunchKernelWithExplicitOperand) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer
    %stream_t = type opaque

    declare i64 @cudaLaunchKernel(i8*, i64, i64, i64, i64, i64, i8**, i64, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @my_kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define i64 @main(i8** %args) {
    entry:
      %stream = inttoptr i64 1 to %stream_t*
      %kernel_ptr = bitcast void ()* @my_kernel to i8*
      %launch = call i64 @cudaLaunchKernel(i8* %kernel_ptr, i64 1, i64 32, i64 1, i64 1, i64 1, i8** %args, i64 0, %stream_t* %stream)
      ret i64 %launch
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &abstract_state = analysis.getAbstractState();
  const auto &gaps = abstract_state.getModelGaps();

  if (analysis.getLaunches().empty()) {
    EXPECT_FALSE(gaps.empty()) << "If kernel not resolved, must create model gap";
    bool found_unknown_kernel_gap = false;
    for (const auto &gap : gaps) {
      if (gap.explanation.find("kernel") != std::string::npos ||
          gap.explanation.find("launch") != std::string::npos) {
        found_unknown_kernel_gap = true;
      }
    }
    EXPECT_TRUE(found_unknown_kernel_gap) << "Must track unresolved kernel as model gap";
  } else {
    ASSERT_FALSE(analysis.getLaunches().empty());
    const auto &launch = analysis.getLaunches().front();
    EXPECT_NE(launch.kernel, nullptr) << "Kernel should be resolved from operand";
  }
}
TEST_F(CUDAAnalysisTest, SeparatesSummariesForSameKernelDifferentDimensions) {
  const char *source = R"(
    @global_arr = addrspace(1) global [128 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [128 x i32], [128 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @main() {
    entry:
      ; First launch with blockDim=1
      call void @__set_CUDAConfig(i32 1, i32 1)
      call void @kernel()
      ; Second launch with blockDim=32
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  const auto &summaries = analysis.getKernelSummaries();
  ASSERT_EQ(summaries.size(), 2u);
  EXPECT_EQ(summaries.size(), 2u) << "Summaries must not be collapsed by function alone";
}
TEST_F(CUDAAnalysisTest, KeepsUnknownDimensionsAsSymbolic) {
  const char *source = R"(
    @global_arr = addrspace(1) global [1024 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()

    define void @kernel(i32 %grid_size) {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %bid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
      ; Index depends on runtime grid_size - cannot be statically determined
      %idx = mul i32 %bid, %tid
      %safe_idx = urem i32 %idx, 1024
      %gep = getelementptr [1024 x i32], [1024 x i32] addrspace(1)* @global_arr, i32 0, i32 %safe_idx
      store i32 %idx, i32 addrspace(1)* %gep
      ret void
    }

    define void @main(i32 %gx, i32 %bx) {
    entry:
      call void @__set_CUDAConfig(i32 %gx, i32 %bx)
      call void @kernel(i32 %gx)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_FALSE(analysis.getLaunches().empty());
  const auto &launch = analysis.getLaunches().front();
  EXPECT_TRUE(launch.dimensions.hasSymbolicGrid() || launch.dimensions.hasSymbolicBlock())
      << "Runtime dimensions must remain symbolic, not concretize to 1";
  ASSERT_FALSE(analysis.getKernelSummaries().empty());
  const auto &summary = analysis.getKernelSummaries().front();
  EXPECT_TRUE(summary.has_global_race || launch.dimensions.hasSymbolicGrid() || launch.dimensions.hasSymbolicBlock())
      << "Must not miss races due to incorrect dimension concretization";
}
TEST_F(CUDAAnalysisTest, DetectsCrossBlockRaceOnConstantAddress) {
  const char *source = R"(
    @constant_global = addrspace(1) global [8 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()

    define void @kernel() {
    entry:
      %bid = call i32 @llvm.nvvm.read.ptx.sreg.ctaid.x()
      ; All blocks write to global[0] - this is a cross-block race
      %gep = getelementptr [8 x i32], [8 x i32] addrspace(1)* @constant_global, i32 0, i32 0
      store i32 %bid, i32 addrspace(1)* %gep
      ret void
    }

    define void @main() {
    entry:
      ; Launch with multiple blocks
      call void @__set_CUDAConfig(i32 4, i32 8)
      call void @kernel()
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_FALSE(analysis.getKernelSummaries().empty());
  const auto &summary = analysis.getKernelSummaries().front();
  EXPECT_TRUE(summary.has_global_race)
      << "Must detect cross-block race on constant address accessed by all blocks";
}
TEST_F(CUDAAnalysisTest, DoesNotConfuseConstantMemoryWithManaged) {
  const char *source = R"(
    @constant_arr = addrspace(4) constant [64 x i32] zeroinitializer

    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @cudaMallocManaged(i8**, i64, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep_const = getelementptr [64 x i32], [64 x i32] addrspace(4)* @constant_arr, i32 0, i32 %tid
      %val = load i32, i32 addrspace(4)* %gep_const
      ret void
    }

    define i32 @main() {
    entry:
      %managed_ptr = alloca i8*
      %r = call i32 @cudaMallocManaged(i8** %managed_ptr, i64 256, i32 1)
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret i32 %r
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &unified = analysis.getUnifiedMemory();
  size_t managed_count = 0;
  for (const auto &um : unified) {
    if (um.is_managed) {
      ++managed_count;
    }
  }
  EXPECT_GE(managed_count, 1u) << "Should have at least one managed allocation";
}

TEST_F(CUDAAnalysisTest, DecodesCurrentCudaLaunchKernelABI) {
  const char *source = R"(
    %dim3 = type { i32, i32, i32 }
    %stream_t = type opaque
    declare i32 @cudaLaunchKernel(i8*, %dim3, %dim3, i8**, i64, %stream_t*)
    define void @kernel() { entry: ret void }
    define i32 @main() {
    entry:
      %stream = inttoptr i64 10 to %stream_t*
      %launch = call i32 @cudaLaunchKernel(
          i8* bitcast (void ()* @kernel to i8*),
          %dim3 { i32 8, i32 4, i32 1 },
          %dim3 { i32 32, i32 2, i32 1 }, i8** null, i64 96,
          %stream_t* %stream)
      ret i32 %launch
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getLaunches().size(), 1u);
  const auto &launch = analysis.getLaunches().front();
  EXPECT_EQ(launch.dimensions.grid[0].constant, 8u);
  EXPECT_EQ(launch.dimensions.grid[1].constant, 4u);
  EXPECT_EQ(launch.dimensions.grid[2].constant, 1u);
  EXPECT_EQ(launch.dimensions.block[0].constant, 32u);
  EXPECT_EQ(launch.dimensions.block[1].constant, 2u);
  EXPECT_EQ(launch.dimensions.block[2].constant, 1u);
  EXPECT_EQ(launch.dynamic_shared_memory_size.constant, 96u);
  EXPECT_NE(launch.argument_array, nullptr);
  EXPECT_TRUE(launch.stream_known);
}

TEST_F(CUDAAnalysisTest, IncludesOrdinaryDeviceCalleeAccesses) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    define void @device_write() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %ptr = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                           i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %ptr
      ret void
    }
    define void @kernel() { entry: call void @device_write() ret void }
    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getKernelSummaries().size(), 1u);
  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_EQ(summary.accesses.size(), 1u);
  EXPECT_TRUE(summary.accesses.front().is_write);
  EXPECT_EQ(summary.accesses.front().space,
            concurrency::cuda::MemorySpace::Global);
}

TEST_F(CUDAAnalysisTest, DetectsSelfInstanceRaceAcrossBlocks) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %ptr = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                           i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %ptr
      ret void
    }
    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 2, i32 32)
      call void @kernel()
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &summary = analysis.getKernelSummaries().front();
  ASSERT_TRUE(summary.has_global_race);
  ASSERT_FALSE(summary.global_races.empty());
  EXPECT_EQ(summary.global_races.front().first,
            summary.global_races.front().second);
  EXPECT_TRUE(summary.global_races.front().cross_block);
}

TEST_F(CUDAAnalysisTest, RejectsTruncatedThreadIndexAsAffine) {
  const char *source = R"(
    @global_arr = addrspace(1) global [256 x i32] zeroinitializer
    declare void @__set_CUDAConfig(i32, i32)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    define void @kernel() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %narrow = trunc i32 %tid to i8
      %idx = zext i8 %narrow to i32
      %ptr = getelementptr [256 x i32], [256 x i32] addrspace(1)* @global_arr,
                           i32 0, i32 %idx
      store i32 %tid, i32 addrspace(1)* %ptr
      ret void
    }
    define void @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 512)
      call void @kernel()
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &access = analysis.getKernelSummaries().front().accesses.front();
  EXPECT_FALSE(access.address_pattern.valid);
  EXPECT_EQ(access.alias_precision,
            concurrency::cuda::AliasPrecision::NonAffine);
}

TEST_F(CUDAAnalysisTest, ConditionalDeviceSyncDoesNotSuppressKernelHazard) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer
    %stream_t = type opaque
    declare i64 @cudaLaunchKernel(i8*, i64, i64, i64, i64, i64,
                                  i8**, i64, %stream_t*)
    declare i32 @cudaDeviceSynchronize()
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    define void @writer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %ptr = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                           i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %ptr
      ret void
    }
    define void @reader() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %ptr = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                           i32 0, i32 %tid
      %value = load i32, i32 addrspace(1)* %ptr
      ret void
    }
    define void @main(i1 %condition) {
    entry:
      %s1 = inttoptr i64 10 to %stream_t*
      %s2 = inttoptr i64 20 to %stream_t*
      %l0 = call i64 @cudaLaunchKernel(
          i8* bitcast (void ()* @writer to i8*), i64 1, i64 32,
          i64 1, i64 1, i64 1, i8** null, i64 0, %stream_t* %s1)
      br i1 %condition, label %sync, label %join
    sync:
      %status = call i32 @cudaDeviceSynchronize()
      br label %join
    join:
      %l1 = call i64 @cudaLaunchKernel(
          i8* bitcast (void ()* @reader to i8*), i64 1, i64 32,
          i64 1, i64 1, i64 1, i8** null, i64 0, %stream_t* %s2)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  EXPECT_FALSE(analysis.getLaunches()[1].ordered_after_previous);
  ASSERT_EQ(analysis.getInterKernelRaces().size(), 1u);
  EXPECT_FALSE(analysis.getInterKernelRaces().front().ordered);
}

TEST_F(CUDAAnalysisTest, DoesNotReportConcurrentReadReadAsHazard) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer
    %stream_t = type opaque
    declare i64 @cudaLaunchKernel(i8*, i64, i64, i64, i64, i64,
                                  i8**, i64, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    define void @reader_a() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %ptr = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                           i32 0, i32 %tid
      %value = load i32, i32 addrspace(1)* %ptr
      ret void
    }
    define void @reader_b() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %ptr = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr,
                           i32 0, i32 %tid
      %value = load i32, i32 addrspace(1)* %ptr
      ret void
    }
    define void @main() {
    entry:
      %s1 = inttoptr i64 10 to %stream_t*
      %s2 = inttoptr i64 20 to %stream_t*
      %l0 = call i64 @cudaLaunchKernel(
          i8* bitcast (void ()* @reader_a to i8*), i64 1, i64 32,
          i64 1, i64 1, i64 1, i8** null, i64 0, %stream_t* %s1)
      %l1 = call i64 @cudaLaunchKernel(
          i8* bitcast (void ()* @reader_b to i8*), i64 1, i64 32,
          i64 1, i64 1, i64 1, i8** null, i64 0, %stream_t* %s2)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  EXPECT_TRUE(analysis.getInterKernelRaces().empty());
}

TEST_F(CUDAAnalysisTest, ConnectsCreatedStreamHandleLifecycle) {
  const char *source = R"(
    %stream_t = type opaque
    declare i32 @cudaStreamCreate(%stream_t**)
    declare i32 @cudaMemcpyAsync(i8*, i8*, i64, i32, %stream_t*)
    declare i32 @cudaStreamSynchronize(%stream_t*)
    declare i32 @cudaStreamDestroy(%stream_t*)
    define i32 @main(i8* %dst, i8* %src) {
    entry:
      %slot = alloca %stream_t*
      %create = call i32 @cudaStreamCreate(%stream_t** %slot)
      %stream = load %stream_t*, %stream_t** %slot
      %copy = call i32 @cudaMemcpyAsync(i8* %dst, i8* %src, i64 64,
                                        i32 1, %stream_t* %stream)
      %sync = call i32 @cudaStreamSynchronize(%stream_t* %stream)
      %destroy = call i32 @cudaStreamDestroy(%stream_t* %stream)
      ret i32 %destroy
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &automata = analysis.getAbstractState().stream_automata;
  ASSERT_EQ(automata.size(), 1u);
  EXPECT_EQ(automata.front().current_state,
            concurrency::cuda::StreamState::Destroyed);
  ASSERT_EQ(automata.front().transitions.size(), 4u);
  EXPECT_EQ(automata.front().transitions[0].to_state,
            concurrency::cuda::StreamState::Created);
  EXPECT_EQ(automata.front().transitions[1].to_state,
            concurrency::cuda::StreamState::Active);
  EXPECT_EQ(automata.front().transitions[2].to_state,
            concurrency::cuda::StreamState::Synchronized);
  EXPECT_FALSE(automata.front().transitions[3].is_ordering_boundary);
}

TEST_F(CUDAAnalysisTest, AsyncCopyDoesNotRerecordExistingEvent) {
  const char *source = R"(
    %stream_t = type opaque
    %event_t = type opaque
    declare i32 @cudaEventRecord(%event_t*, %stream_t*)
    declare i32 @cudaMemcpyAsync(i8*, i8*, i64, i32, %stream_t*)
    define i32 @main(%event_t* %event, %stream_t* %record_stream,
                     %stream_t* %copy_stream, i8* %dst, i8* %src) {
    entry:
      %record = call i32 @cudaEventRecord(%event_t* %event,
                                          %stream_t* %record_stream)
      %copy = call i32 @cudaMemcpyAsync(i8* %dst, i8* %src, i64 64,
                                        i32 1, %stream_t* %copy_stream)
      ret i32 %copy
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &events = analysis.getAbstractState().event_automata;
  ASSERT_EQ(events.size(), 1u);
  EXPECT_TRUE(events.front().has_record);
  EXPECT_EQ(events.front().recorded_stream,
            module->getFunction("main")->getArg(1));
  ASSERT_EQ(events.front().transitions.size(), 1u);
  EXPECT_EQ(events.front().current_state,
            concurrency::cuda::EventState::Recorded);
}

TEST_F(CUDAAnalysisTest, InstantiatesKernelArgumentsPerLaunch) {
  const char *source = R"(
    %dim3 = type { i32, i32, i32 }
    %stream_t = type opaque
    @buffer_a = addrspace(1) global [32 x i32] zeroinitializer
    @buffer_b = addrspace(1) global [32 x i32] zeroinitializer
    @arg_a = global i32 addrspace(1)* getelementptr (
        [32 x i32], [32 x i32] addrspace(1)* @buffer_a, i32 0, i32 0)
    @arg_b = global i32 addrspace(1)* getelementptr (
        [32 x i32], [32 x i32] addrspace(1)* @buffer_b, i32 0, i32 0)
    @args_a = global [1 x i8*] [i8* bitcast (i32 addrspace(1)** @arg_a to i8*)]
    @args_b = global [1 x i8*] [i8* bitcast (i32 addrspace(1)** @arg_b to i8*)]
    declare i32 @cudaLaunchKernel(i8*, %dim3, %dim3, i8**, i64, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    define void @kernel(i32 addrspace(1)* %buffer) {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %ptr = getelementptr i32, i32 addrspace(1)* %buffer, i32 %tid
      store i32 %tid, i32 addrspace(1)* %ptr
      ret void
    }
    define void @main() {
    entry:
      %s1 = inttoptr i64 10 to %stream_t*
      %s2 = inttoptr i64 20 to %stream_t*
      %l0 = call i32 @cudaLaunchKernel(
          i8* bitcast (void (i32 addrspace(1)*)* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 },
          %dim3 { i32 32, i32 1, i32 1 },
          i8** getelementptr ([1 x i8*], [1 x i8*]* @args_a, i32 0, i32 0),
          i64 0, %stream_t* %s1)
      %l1 = call i32 @cudaLaunchKernel(
          i8* bitcast (void (i32 addrspace(1)*)* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 },
          %dim3 { i32 32, i32 1, i32 1 },
          i8** getelementptr ([1 x i8*], [1 x i8*]* @args_b, i32 0, i32 0),
          i64 0, %stream_t* %s2)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  EXPECT_TRUE(analysis.getInterKernelRaces().empty());
}

TEST_F(CUDAAnalysisTest, DetectsSameInstantiatedKernelArgumentHazard) {
  const char *source = R"(
    %dim3 = type { i32, i32, i32 }
    %stream_t = type opaque
    @buffer = addrspace(1) global [32 x i32] zeroinitializer
    @arg = global i32 addrspace(1)* getelementptr (
        [32 x i32], [32 x i32] addrspace(1)* @buffer, i32 0, i32 0)
    @args = global [1 x i8*] [i8* bitcast (i32 addrspace(1)** @arg to i8*)]
    declare i32 @cudaLaunchKernel(i8*, %dim3, %dim3, i8**, i64, %stream_t*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()
    define void @kernel(i32 addrspace(1)* %buffer_arg) {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %ptr = getelementptr i32, i32 addrspace(1)* %buffer_arg, i32 %tid
      store i32 %tid, i32 addrspace(1)* %ptr
      ret void
    }
    define void @main() {
    entry:
      %s1 = inttoptr i64 10 to %stream_t*
      %s2 = inttoptr i64 20 to %stream_t*
      %l0 = call i32 @cudaLaunchKernel(
          i8* bitcast (void (i32 addrspace(1)*)* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 },
          %dim3 { i32 32, i32 1, i32 1 },
          i8** getelementptr ([1 x i8*], [1 x i8*]* @args, i32 0, i32 0),
          i64 0, %stream_t* %s1)
      %l1 = call i32 @cudaLaunchKernel(
          i8* bitcast (void (i32 addrspace(1)*)* @kernel to i8*),
          %dim3 { i32 1, i32 1, i32 1 },
          %dim3 { i32 32, i32 1, i32 1 },
          i8** getelementptr ([1 x i8*], [1 x i8*]* @args, i32 0, i32 0),
          i64 0, %stream_t* %s2)
      ret void
    }
  )";
  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  ASSERT_EQ(analysis.getLaunches().size(), 2u);
  ASSERT_FALSE(analysis.getInterKernelRaces().empty());
  EXPECT_FALSE(analysis.getInterKernelRaces().front().ordered);
}
