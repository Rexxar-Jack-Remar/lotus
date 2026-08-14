TEST_F(MPIAnalysisTest, OpenMPIInternalCollectiveNamesAreRecognized) {
  const char *source = R"(
    declare i32 @ompi_mpi_bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @ompi_mpi_bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(MPIOpKind::COLLECTIVE_BLOCKING), 1u);
}

TEST_F(MPIAnalysisTest, SemanticDescriptorCoverageForCoreMPITypes) {
  const std::vector<ThreadAPI::TD_TYPE> required_types = {
      ThreadAPI::TD_MPI_INIT,
      ThreadAPI::TD_MPI_FINALIZE,
      ThreadAPI::TD_MPI_SEND,
      ThreadAPI::TD_MPI_RECV,
      ThreadAPI::TD_MPI_SENDRECV,
      ThreadAPI::TD_MPI_ISEND,
      ThreadAPI::TD_MPI_IRECV,
      ThreadAPI::TD_MPI_WAIT,
      ThreadAPI::TD_MPI_TEST,
      ThreadAPI::TD_MPI_BARRIER,
      ThreadAPI::TD_MPI_BCAST,
      ThreadAPI::TD_MPI_REDUCE,
      ThreadAPI::TD_MPI_WIN_CREATE,
      ThreadAPI::TD_MPI_PUT,
      ThreadAPI::TD_MPI_WIN_LOCK,
      ThreadAPI::TD_MPI_COMM_DUP,
      ThreadAPI::TD_MPI_COMM_CREATE,
      ThreadAPI::TD_MPI_REQUEST_FREE,
      ThreadAPI::TD_MPI_TYPE_CONTIGUOUS,
      ThreadAPI::TD_MPI_TYPE_COMMIT,
      ThreadAPI::TD_MPI_SESSION_INIT,
      ThreadAPI::TD_MPI_SESSION_FINALIZE,
  };

  for (ThreadAPI::TD_TYPE type : required_types) {
    const MPISemanticDescriptor *descriptor = lookupMPISemantic(type);
    ASSERT_NE(descriptor, nullptr);
    if (type == ThreadAPI::TD_MPI_SEND || type == ThreadAPI::TD_MPI_RECV ||
        type == ThreadAPI::TD_MPI_ISEND || type == ThreadAPI::TD_MPI_IRECV) {
      EXPECT_NE(descriptor->communicator_arg, -1);
      EXPECT_NE(descriptor->count_arg, -1);
      EXPECT_NE(descriptor->datatype_arg, -1);
      EXPECT_NE(descriptor->peer_rank_arg, -1);
      EXPECT_NE(descriptor->tag_arg, -1);
    }
    if (type == ThreadAPI::TD_MPI_WAIT || type == ThreadAPI::TD_MPI_WAITALL ||
        type == ThreadAPI::TD_MPI_TEST || type == ThreadAPI::TD_MPI_TESTALL) {
      EXPECT_NE(descriptor->request_arg, -1);
    }
  }
}

TEST_F(MPIAnalysisTest,
       SemanticDescriptorCoverageForNewStatusAndTopologyTypes) {
  const MPISemanticDescriptor *get_count =
      lookupMPISemantic(ThreadAPI::TD_MPI_GET_COUNT);
  ASSERT_NE(get_count, nullptr);
  EXPECT_EQ(get_count->kind, MPIOpKind::REQUEST_MANAGEMENT);
  EXPECT_EQ(get_count->family, MPISemanticFamily::Request);

  const MPISemanticDescriptor *status_set =
      lookupMPISemantic(ThreadAPI::TD_MPI_STATUS_SET_ELEMENTS);
  ASSERT_NE(status_set, nullptr);
  EXPECT_EQ(status_set->kind, MPIOpKind::REQUEST_MANAGEMENT);
  EXPECT_EQ(status_set->family, MPISemanticFamily::Request);

  const MPISemanticDescriptor *cart_create =
      lookupMPISemantic(ThreadAPI::TD_MPI_CART_CREATE);
  ASSERT_NE(cart_create, nullptr);
  EXPECT_EQ(cart_create->kind, MPIOpKind::COMM_MANAGEMENT);
  EXPECT_EQ(cart_create->family, MPISemanticFamily::Communicator);

  const MPISemanticDescriptor *dist_graph_neighbors =
      lookupMPISemantic(ThreadAPI::TD_MPI_DIST_GRAPH_NEIGHBORS);
  ASSERT_NE(dist_graph_neighbors, nullptr);
  EXPECT_EQ(dist_graph_neighbors->kind, MPIOpKind::COMM_MANAGEMENT);
  EXPECT_EQ(dist_graph_neighbors->family, MPISemanticFamily::Communicator);

  const MPISemanticDescriptor *graph_map =
      lookupMPISemantic(ThreadAPI::TD_MPI_GRAPH_MAP);
  ASSERT_NE(graph_map, nullptr);
  EXPECT_EQ(graph_map->kind, MPIOpKind::COMM_MANAGEMENT);
  EXPECT_EQ(graph_map->family, MPISemanticFamily::Communicator);
}

TEST_F(MPIAnalysisTest, MPISpecDescriptorsAreExplicitlyCovered) {
  std::ifstream spec(
      "/Users/rainoftime/Work/analysis/lotus/config/mpi_api.spec");
  ASSERT_TRUE(spec.is_open());

  auto isExplicitlyIgnored = [](const std::string &type_name) {
    return type_name.find("ERRHANDLER") != std::string::npos ||
           type_name.find("INFO_") != std::string::npos ||
           type_name.find("STATUS_") != std::string::npos ||
           type_name.find("ERROR_") != std::string::npos ||
           type_name.find("GRAPH_") != std::string::npos ||
           type_name.find("CART_") != std::string::npos ||
           type_name.find("DIST_GRAPH_") != std::string::npos ||
           type_name.find("GET_COUNT") != std::string::npos ||
           type_name.find("GET_ELEMENTS") != std::string::npos;
  };

  std::string line;
  size_t checked = 0;
  while (std::getline(spec, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::istringstream iss(line);
    std::string symbol;
    std::string type_name;
    if (!(iss >> symbol >> type_name)) {
      continue;
    }
    if (type_name.find("TD_MPI_") != 0) {
      continue;
    }

    LLVMContext local_context;
    Module local_module("mpi_spec_probe", local_context);
    auto *fn_ty = FunctionType::get(Type::getInt32Ty(local_context), false);
    Function *fn = Function::Create(fn_ty, Function::ExternalLinkage, symbol,
                                    local_module);
    ThreadAPI::TD_TYPE type = ThreadAPI::getThreadAPI()->getType(fn);
    if (type == ThreadAPI::TD_DUMMY) {
      continue;
    }
    if (isExplicitlyIgnored(type_name)) {
      continue;
    }

    EXPECT_NE(lookupMPISemantic(type), nullptr)
        << "missing descriptor for " << symbol << " " << type_name;
    ++checked;
  }

  EXPECT_GT(checked, 50u);
}

TEST_F(MPIAnalysisTest, NeighborCollectivesUseDistinctProtocolClass) {
  const char *source = R"(
    declare i32 @MPI_Alltoall(i8*, i32, i32, i8*, i32, i32, i8*)
    declare i32 @MPI_Neighbor_alltoall(i8*, i32, i32, i8*, i32, i32, i8*)

    define void @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Alltoall(i8* null, i32 1, i32 2, i8* null, i32 1, i32 2, i8* %comm)
      ret void
    }

    define void @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Neighbor_alltoall(i8* null, i32 4, i32 2, i8* null, i32 4, i32 2, i8* %comm)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().mismatched_collectives.size(), 0u);
}

TEST_F(MPIAnalysisTest, IntercommunicatorCreateGetsDedicatedOperationKind) {
  const char *source = R"(
    declare i32 @MPI_Intercomm_create(i8*, i32, i8*, i32, i32, i8**)

    define i32 @main(i8* %local, i8* %peer) {
    entry:
      %out = alloca i8*, align 8
      call i32 @MPI_Intercomm_create(i8* %local, i32 0, i8* %peer, i32 0, i32 7, i8** %out)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(MPIOpKind::INTERCOMM_CREATION), 1u);
}

TEST_F(MPIAnalysisTest, DerivedDatatypeExtentPropagatesToPointToPointOps) {
  const char *source = R"(
    declare i32 @MPI_Type_contiguous(i32, i32, i8*)
    declare i32 @MPI_Send(i8*, i32, i8*, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %derived_type = alloca i8, align 1
      call i32 @MPI_Type_contiguous(i32 4, i32 2, i8* %derived_type)
      call i32 @MPI_Send(i8* null, i32 3, i8* %derived_type, i32 1, i32 7, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto sends =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::SEND_BLOCKING);
  ASSERT_EQ(sends.size(), 1u);
  EXPECT_EQ(sends.front().datatype_size, 16);
  EXPECT_EQ(sends.front().byte_length, 48);
}

TEST_F(MPIAnalysisTest, WrappedCommRankStillRefinesConditionalCollectives) {
  const char *source = R"(
    declare i32 @__wrap_MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @__wrap_MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %is_root = icmp eq i32 %loaded, 0
      br i1 %is_root, label %then, label %done

    then:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      br label %done

    done:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().conditional_collectives.size(), 1u);
}
