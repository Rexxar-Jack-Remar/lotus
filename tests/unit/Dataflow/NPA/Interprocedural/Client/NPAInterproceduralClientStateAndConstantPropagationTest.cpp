/**
 * @file NPAInterproceduralClientStateAndConstantPropagationTest.cpp
 * @brief Interprocedural NPA clients: reaching definitions, MaybeUninitialized, block-entry hooks, bounded-solver status, constant propagation, and projected summaries.
 */

#include "NPAInterproceduralClientTestSupport.h"

TEST(NPAInterproceduralClients,
     SparseReachingDefinitionFactsMatchDenseNewtonRounds) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @main() {
    entry:
      %first = add i32 1, 2
      br label %next
    next:
      %second = add i32 %first, 3
      ret i32 %second
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  auto nextIt = std::next(main->begin());
  ASSERT_NE(nextIt, main->end());
  auto *next = &*nextIt;

  auto dense = npa::InterReachingDefinitions::run(*module);
  auto sparse = npa::InterReachingDefinitions::run(
      *module, false, npa::LinearStrategy::SCC,
      npa::IndirectCallResolutionMode::ClosedWorldTypeCompatible,
      npa::NewtonRoundStrategy::Sparse);
  ASSERT_TRUE(dense.status.overall_converged);
  ASSERT_TRUE(sparse.status.overall_converged);

  const auto &denseFact = dense.blockFacts.at(npa::BlockKey{next});
  const auto &sparseFact = sparse.blockFacts.at(npa::BlockKey{next});
  EXPECT_EQ(denseFact, sparseFact);
  EXPECT_TRUE(sparseFact.test(0));
  EXPECT_EQ(sparseFact.count(), 1u);
}

TEST(NPAInterproceduralClients, MaybeUninitializedFlowsThroughCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %p = alloca i32
      %r = call i32 @id(i32* %p)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);

  auto result = npa::InterMaybeUninitialized::run(*module);
  auto entryFact =
      unionFactForBlock(result.blockFacts, &Id->getEntryBlock());
  EXPECT_GT(entryFact.count(), 0u);
}

TEST(NPAInterproceduralClients, MaybeUninitializedStoreClearsBeforeCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32* %p) {
    entry:
      %v = load i32, i32* %p
      ret i32 %v
    }

    define i32 @main() {
    entry:
      %p = alloca i32
      store i32 7, i32* %p
      %r = call i32 @id(i32* %p)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);

  auto result = npa::InterMaybeUninitialized::run(*module);
  auto entryFact =
      unionFactForBlock(result.blockFacts, &Id->getEntryBlock());
  EXPECT_EQ(entryFact.count(), 0u);
}

TEST(NPAInterproceduralClients,
     GenericBlockEntryHookAppliesToEntryAndSuccessorBlocks) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      br label %mid

    mid:
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  EntryHookAnalysis analysis;
  auto result = npa::InterEngine<EntryHookDomain, EntryHookAnalysis>::run(
      *module, analysis, false, npa::LinearStrategy::SCC);

  // Transfer expressions are immutable artifacts. Fact propagation reuses
  // their prepared block/call summaries instead of rebuilding every
  // instruction transfer when a function is dequeued.
  EXPECT_EQ(analysis.getTransferBuilds(), 3u);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto EntryIt = Main->begin();
  auto MidIt = std::next(Main->begin(), 1);
  auto ExitIt = std::next(Main->begin(), 2);
  ASSERT_NE(ExitIt, Main->end());

  auto entryStates = statesForBlock(result.blockEntryFacts, &*EntryIt);
  auto midStates = statesForBlock(result.blockEntryFacts, &*MidIt);
  auto exitStates = statesForBlock(result.blockEntryFacts, &*ExitIt);
  ASSERT_EQ(entryStates.size(), 1u);
  ASSERT_EQ(midStates.size(), 1u);
  ASSERT_EQ(exitStates.size(), 1u);

  EXPECT_TRUE(containsPath(*entryStates.front(), {'E'}));
  EXPECT_TRUE(containsPath(*midStates.front(), {'E', 'M'}));
  EXPECT_TRUE(containsPath(*exitStates.front(), {'E', 'M', 'E'}));
}

TEST(NPAInterproceduralClients,
     InterproceduralEngineReportsBoundedSolverResults) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  LimitedBoolAnalysis analysis;
  auto result = npa::InterEngine<LimitedBoolDomain, LimitedBoolAnalysis>::run(
      *module, analysis, false, npa::LinearStrategy::SCC);

  EXPECT_FALSE(result.status.summary_solve.converged);
  EXPECT_TRUE(result.status.summary_solve.hit_limit);
  EXPECT_TRUE(result.status.summary_solve.hit_linear_limit);
  EXPECT_FALSE(result.status.summary_solve.hit_fixpoint_limit);
  EXPECT_TRUE(result.status.used_bounded_inner_solve);
  EXPECT_FALSE(result.status.overall_converged);
  EXPECT_TRUE(result.status.overall_hit_limit);
  EXPECT_TRUE(result.status.approximated);
}

TEST(NPAInterproceduralClients,
     ForwardEngineSeparatesExactSummarySolveFromPropagationLimitOnRecursion) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @recur() {
    entry:
      call void @recur()
      ret void
    }

    define void @main() {
    entry:
      call void @recur()
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  RecursivePropagationLimitedForwardAnalysis analysis;
  auto result = npa::InterEngine<RecursiveSummaryDomain,
                                 RecursivePropagationLimitedForwardAnalysis>::
      run(*module, analysis, false, npa::LinearStrategy::SCC);

  auto *Recur = module->getFunction("recur");
  ASSERT_NE(Recur, nullptr);
  EXPECT_TRUE(result.status.summary_solve.converged);
  EXPECT_FALSE(result.status.summary_solve.hit_limit);
  EXPECT_FALSE(result.status.used_bounded_inner_solve);
  EXPECT_FALSE(result.status.propagation_converged);
  EXPECT_TRUE(result.status.propagation_hit_limit);
  EXPECT_FALSE(result.status.overall_converged);
  EXPECT_TRUE(result.status.approximated);
  EXPECT_NE(result.summaries.find(npa::FunctionKey{Recur}),
            result.summaries.end());
}

TEST(NPAInterproceduralClients,
     ConstantPropagationTransfersArgumentsAcrossCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @add2(i32 %x) {
    entry:
      %y = add i32 %x, 2
      ret i32 %y
    }

    define i32 @main() {
    entry:
      %r = call i32 @add2(i32 5)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Add2 = module->getFunction("add2");
  ASSERT_NE(Add2, nullptr);
  auto *Arg = &*Add2->arg_begin();

  auto result = npa::InterConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, &Add2->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto It = states.front()->values.find(Arg);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 5));
}

TEST(NPAInterproceduralClients, ConstantPropagationReportsSummaryOverflow) {
  llvm::LLVMContext ctx;
  std::ostringstream ir;
  ir << "define i32 @chain(i32 %x) {\n";
  ir << "entry:\n";
  ir << "  br label %b0\n";
  std::string current = "%x";
  for (unsigned i = 0; i < 321; ++i) {
    ir << "b" << i << ":\n";
    const std::string next = "%v" + std::to_string(i);
    ir << "  " << next << " = add i32 " << current << ", 1\n";
    ir << "  br label %" << (i == 320 ? "exit" : "b" + std::to_string(i + 1))
       << "\n";
    current = next;
  }
  ir << "exit:\n";
  ir << "  ret i32 " << current << "\n";
  ir << "}\n\n";
  ir << "define i32 @main() {\n";
  ir << "entry:\n";
  ir << "  %r = call i32 @chain(i32 0)\n";
  ir << "  ret i32 %r\n";
  ir << "}\n";

  const std::string moduleText = ir.str();
  auto module = parseModule(ctx, moduleText.c_str());
  ASSERT_NE(module, nullptr);

  auto result = npa::InterConstantPropagation::run(*module);

  EXPECT_TRUE(result.status.summary_solve.converged);
  EXPECT_TRUE(result.status.used_summary_overflow);
  EXPECT_FALSE(result.status.used_fact_widening);
  EXPECT_TRUE(result.status.approximated);
  EXPECT_FALSE(result.status.overall_converged);
}

TEST(NPAInterproceduralClients,
     InterproceduralClientsAcceptTensorStrategyOnDemand) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %r = call i32 @id(i32 5)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);
  auto *Arg = &*Id->arg_begin();

  auto result = npa::InterConstantPropagation::run(
      *module, false, npa::LinearStrategy::TensorProduct);
  EXPECT_TRUE(result.status.summary_solve.converged);
  EXPECT_FALSE(result.status.summary_solve.hit_limit);
  EXPECT_FALSE(result.status.used_bounded_inner_solve);
  EXPECT_TRUE(result.status.overall_converged);
  EXPECT_FALSE(result.status.overall_hit_limit);
  EXPECT_FALSE(result.status.approximated);
  auto states = statesForBlock(result.blockFacts, &Id->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto It = states.front()->values.find(Arg);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 5));
}

TEST(NPAInterproceduralClients,
     PublicSummariesRemainUnprojectedWhenDomainProjects) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @callee() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @callee()
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  ProjectedSummaryAnalysis analysis;
  auto result =
      npa::InterEngine<ProjectedStringDomain, ProjectedSummaryAnalysis>::run(
          *module, analysis, false, npa::LinearStrategy::SCC);

  auto *Callee = module->getFunction("callee");
  ASSERT_NE(Callee, nullptr);
  auto it = result.summaries.find(npa::FunctionKey{Callee});
  ASSERT_NE(it, result.summaries.end());
  EXPECT_EQ(it->second, (ProjectedStringDomain::value_type{"l"}));
}

TEST(NPAInterproceduralClients,
     DomainsWithProjectOnlyProjectCalleeSummariesAtCallSites) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @callee() {
    entry:
      ret void
    }

    define void @main() {
    entry:
      call void @callee()
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  ProjectedSummaryAnalysis analysis;
  auto result =
      npa::InterEngine<ProjectedStringDomain, ProjectedSummaryAnalysis>::run(
          *module, analysis, false, npa::LinearStrategy::SCC);

  auto *Main = module->getFunction("main");
  auto *Callee = module->getFunction("callee");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Callee, nullptr);

  auto main_it = result.summaries.find(npa::FunctionKey{Main});
  auto callee_it = result.summaries.find(npa::FunctionKey{Callee});
  ASSERT_NE(main_it, result.summaries.end());
  ASSERT_NE(callee_it, result.summaries.end());

  EXPECT_EQ(callee_it->second, (ProjectedStringDomain::value_type{"l"}));
  EXPECT_EQ(main_it->second, (ProjectedStringDomain::value_type{""}));
}

TEST(NPAInterproceduralClients,
     PredicateRelationTensorStrategyPreservesProjectedLoopSummarySemantics) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @callee(i1 %cond) {
    entry:
      br label %loop

    loop:
      %set_local = xor i1 %cond, false
      br i1 %cond, label %loop, label %exit

    exit:
      ret void
    }

    define void @main(i1 %cond) {
    entry:
      call void @callee(i1 %cond)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  npa::PredicateRelationDomain::configure(2, 1);
  PredicateProjectedLoopAnalysis analysis;

  auto worklist = npa::InterEngine<
      npa::PredicateRelationDomain,
      PredicateProjectedLoopAnalysis>::run(*module, analysis, false,
                                           npa::LinearStrategy::SCC);

  auto tensor = npa::InterEngine<
      npa::PredicateRelationDomain,
      PredicateProjectedLoopAnalysis>::run(*module, analysis, false,
                                           npa::LinearStrategy::TensorProduct);

  auto *Main = module->getFunction("main");
  auto *Callee = module->getFunction("callee");
  ASSERT_NE(Main, nullptr);
  ASSERT_NE(Callee, nullptr);

  auto worklist_main = worklist.summaries.find(npa::FunctionKey{Main});
  auto worklist_callee = worklist.summaries.find(npa::FunctionKey{Callee});
  auto tensor_main = tensor.summaries.find(npa::FunctionKey{Main});
  auto tensor_callee = tensor.summaries.find(npa::FunctionKey{Callee});
  ASSERT_NE(worklist_main, worklist.summaries.end());
  ASSERT_NE(worklist_callee, worklist.summaries.end());
  ASSERT_NE(tensor_main, tensor.summaries.end());
  ASSERT_NE(tensor_callee, tensor.summaries.end());

  EXPECT_TRUE(npa::PredicateRelationDomain::equal(worklist_main->second,
                                                  tensor_main->second));
  EXPECT_TRUE(npa::PredicateRelationDomain::equal(worklist_callee->second,
                                                  tensor_callee->second));
  EXPECT_EQ(sortedPredicateTransitions(tensor_main->second),
            sortedPredicateTransitions(npa::PredicateRelationDomain::one()));
  EXPECT_EQ(sortedPredicateTransitions(tensor_callee->second),
            (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                {0, 2}, {1, 3}, {2, 2}, {3, 3}}));
}

TEST(NPAInterproceduralClients,
     TensorStrategyFallsBackForNonAdmissibleInterproceduralDomains) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %r = call i32 @id(i32 5)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  testing::internal::CaptureStderr();
  auto result = npa::InterConstantPropagation::run(
      *module, true, npa::LinearStrategy::TensorProduct);
  std::string stderrOutput = testing::internal::GetCapturedStderr();

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);
  auto states = statesForBlock(result.blockFacts, &Id->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto *Arg = &*Id->arg_begin();
  auto It = states.front()->values.find(Arg);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 5));
  EXPECT_NE(stderrOutput.find("falling back"), std::string::npos);
}

TEST(NPAInterproceduralClients, ConstantPropagationUsesLLVMIntegerSemantics) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %z = zext i1 true to i32
      %t = trunc i32 256 to i8
      %cmp = icmp ugt i32 -1, 1
      %q = udiv i32 -1, 2
      %s = lshr i32 -1, 1
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Z = findInstructionByName(*Main, "z");
  auto *T = findInstructionByName(*Main, "t");
  auto *Cmp = findInstructionByName(*Main, "cmp");
  auto *Q = findInstructionByName(*Main, "q");
  auto *S = findInstructionByName(*Main, "s");
  ASSERT_NE(Z, nullptr);
  ASSERT_NE(T, nullptr);
  ASSERT_NE(Cmp, nullptr);
  ASSERT_NE(Q, nullptr);
  ASSERT_NE(S, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto ZIt = states.front()->values.find(Z);
  auto TIt = states.front()->values.find(T);
  auto CmpIt = states.front()->values.find(Cmp);
  auto QIt = states.front()->values.find(Q);
  auto SIt = states.front()->values.find(S);
  ASSERT_NE(ZIt, states.front()->values.end());
  ASSERT_NE(TIt, states.front()->values.end());
  ASSERT_NE(CmpIt, states.front()->values.end());
  ASSERT_NE(QIt, states.front()->values.end());
  ASSERT_NE(SIt, states.front()->values.end());

  expectConstValue(ZIt->second, unsignedAPInt(32, 1));
  expectConstValue(TIt->second, signedAPInt(8, 0));
  expectConstValue(CmpIt->second, unsignedAPInt(1, 1));
  expectConstValue(QIt->second, unsignedAPInt(32, 2147483647));
  expectConstValue(SIt->second, unsignedAPInt(32, 2147483647));
}

TEST(NPAInterproceduralClients,
     ConstantPropagationSupportsLargeUnsignedValues) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %q = udiv i64 -1, 1
      %s = lshr i64 -1, 0
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Q = findInstructionByName(*Main, "q");
  auto *S = findInstructionByName(*Main, "s");
  ASSERT_NE(Q, nullptr);
  ASSERT_NE(S, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto QIt = states.front()->values.find(Q);
  auto SIt = states.front()->values.find(S);
  ASSERT_NE(QIt, states.front()->values.end());
  ASSERT_NE(SIt, states.front()->values.end());
  expectConstValue(QIt->second,
                   unsignedAPInt(64, std::numeric_limits<uint64_t>::max()));
  expectConstValue(SIt->second,
                   unsignedAPInt(64, std::numeric_limits<uint64_t>::max()));
}
