TEST(NPAInterproceduralClients,
     ConstantPropagationPreservesPhiConditionCorrelation) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right

    left:
      br label %merge

    right:
      br label %merge

    merge:
      %x = phi i32 [ 1, %left ], [ 2, %right ]
      %c = zext i1 %cond to i32
      %y = add i32 %x, %c
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Y = findInstructionByName(*Main, "y");
  ASSERT_NE(Y, nullptr);
  auto NextIt = std::next(Main->begin(), 4);
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Y);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 2));
}

TEST(NPAInterproceduralClients,
     ConstantPropagationResolvesIndirectSingleTargetCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %fp = select i1 true, i32 (i32)* @id, i32 (i32)* @id
      %r = call i32 %fp(i32 7)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);
  auto *Arg = &*Id->arg_begin();

  auto result = npa::InterConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, &Id->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Arg);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 7));
}

TEST(NPAInterproceduralClients,
     CallResolutionModeControlsIndirectCalleeDiscovery) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %fp = select i1 true, i32 (i32)* @id, i32 (i32)* @id
      %r = call i32 %fp(i32 9)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);

  EntryHookAnalysis analysis;
  auto closedWorld =
      npa::InterEngine<EntryHookDomain, EntryHookAnalysis>::run(
          *module, analysis, false, npa::LinearStrategy::SCC,
          npa::IndirectCallResolutionMode::ClosedWorldTypeCompatible);
  auto fallbackOnly =
      npa::InterEngine<EntryHookDomain, EntryHookAnalysis>::run(
          *module, analysis, false, npa::LinearStrategy::SCC,
          npa::IndirectCallResolutionMode::DeclaredOnlyFallback);

  EXPECT_EQ(closedWorld.status.call_resolution_mode,
            npa::IndirectCallResolutionMode::ClosedWorldTypeCompatible);
  EXPECT_EQ(fallbackOnly.status.call_resolution_mode,
            npa::IndirectCallResolutionMode::DeclaredOnlyFallback);
  EXPECT_GE(closedWorld.status.indirect_calls_seen, 1);
  EXPECT_GE(fallbackOnly.status.indirect_calls_seen, 1);
  EXPECT_EQ(closedWorld.status.unresolved_indirect_calls, 0);
  EXPECT_GE(fallbackOnly.status.unresolved_indirect_calls, 1);

  auto closedStates =
      statesForBlock(closedWorld.blockEntryFacts, &Id->getEntryBlock());
  auto fallbackStates =
      statesForBlock(fallbackOnly.blockEntryFacts, &Id->getEntryBlock());
  EXPECT_EQ(closedStates.size(), 1u);
  EXPECT_TRUE(fallbackStates.empty());
}

TEST(NPAInterproceduralClients,
     ConstantPropagationWrapperExposesCallResolutionMode) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %fp = select i1 true, i32 (i32)* @id, i32 (i32)* @id
      %r = call i32 %fp(i32 1)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto result = npa::InterConstantPropagation::run(
      *module, false, npa::LinearStrategy::SCC,
      npa::IndirectCallResolutionMode::DeclaredOnlyFallback);
  EXPECT_EQ(result.status.call_resolution_mode,
            npa::IndirectCallResolutionMode::DeclaredOnlyFallback);
  EXPECT_GE(result.status.unresolved_indirect_calls, 1);
}

TEST(NPAInterproceduralClients,
     ConstantPropagationResolvesCompatibleBitcastedIndirectTargets) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @inc(i32 %x) {
    entry:
      %y = add i32 %x, 1
      ret i32 %y
    }

    define i32 @main(i1 %cond) {
    entry:
      %id.cast = bitcast i32 (i32)* @id to i32 (...)*
      %inc.cast = bitcast i32 (i32)* @inc to i32 (...)*
      %fp = select i1 %cond, i32 (...)* %id.cast, i32 (...)* %inc.cast
      %r = call i32 (...) %fp(i32 7)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  auto *Inc = module->getFunction("inc");
  ASSERT_NE(Id, nullptr);
  ASSERT_NE(Inc, nullptr);

  auto *IdArg = &*Id->arg_begin();
  auto *IncArg = &*Inc->arg_begin();

  auto result = npa::InterConstantPropagation::run(*module);
  auto idStates = statesForBlock(result.blockFacts, &Id->getEntryBlock());
  auto incStates = statesForBlock(result.blockFacts, &Inc->getEntryBlock());
  ASSERT_EQ(idStates.size(), 1u);
  ASSERT_EQ(incStates.size(), 1u);

  auto IdIt = idStates.front()->values.find(IdArg);
  auto IncIt = incStates.front()->values.find(IncArg);
  ASSERT_NE(IdIt, idStates.front()->values.end());
  ASSERT_NE(IncIt, incStates.front()->values.end());
  expectConstValue(IdIt->second, signedAPInt(32, 7));
  expectConstValue(IncIt->second, signedAPInt(32, 7));
}

TEST(NPAInterproceduralClients,
     ConstantPropagationRejectsIncompatibleBitcastedDirectCallee) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @bad(i8* %p) {
    entry:
      ret i32 42
    }

    define void @main() {
    entry:
      %fp = bitcast i32 (i8*)* @bad to i32 (i32)*
      %r = call i32 %fp(i32 7)
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *R = findInstructionByName(*Main, "r");
  ASSERT_NE(R, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(R);
  EXPECT_EQ(It, states.front()->values.end());
}

TEST(NPAInterproceduralClients,
     ConstantPropagationUsesIndirectCallTargetsWhenDiscoveringEntries) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @caller() {
    entry:
      %fp = bitcast i32 (i32)* @id to i32 (...)*
      %r = call i32 (...) %fp(i32 7)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);
  auto *Arg = &*Id->arg_begin();

  auto result = npa::InterConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, &Id->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Arg);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 7));
}

TEST(NPAInterproceduralClients,
     ConstantPropagationDefaultSwitchCanBeUnreachable) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      switch i32 3, label %default [ i32 1, label %case1
                                     i32 3, label %case3 ]

    case1:
      %x1 = add i32 1, 1
      br label %join

    case3:
      %x3 = add i32 4, 5
      br label %join

    default:
      %xd = add i32 7, 8
      br label %join

    join:
      %x = phi i32 [ %x1, %case1 ], [ %x3, %case3 ], [ %xd, %default ]
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = findInstructionByName(*Main, "x");
  ASSERT_NE(X, nullptr);
  auto NextIt = std::next(Main->begin(), 4);
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterConstantPropagation::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);
  auto It = states.front()->values.find(X);
  ASSERT_NE(It, states.front()->values.end());
  expectConstValue(It->second, signedAPInt(32, 9));
}

TEST(NPAInterproceduralClients, IntervalAnalysisJoinsAtSingleFunctionEntry) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @id(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @main() {
    entry:
      %a = call i32 @id(i32 2)
      %b = call i32 @id(i32 10)
      %c = add i32 %a, %b
      ret i32 %c
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Id = module->getFunction("id");
  ASSERT_NE(Id, nullptr);
  auto *Arg = &*Id->arg_begin();

  auto result = npa::InterIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, &Id->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto It = states.front()->values.find(Arg);
  ASSERT_NE(It, states.front()->values.end());
  expectIntervalRange(It->second, signedAPInt(32, 2), signedAPInt(32, 10),
                      npa::IntervalOrdering::Signed);
}

TEST(NPAInterproceduralClients, LiveVariablesFlowBackThroughCall) {
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

  auto result = npa::InterLiveVariables::run(*module);
  auto BitIt = result.valueBits.find(Arg);
  ASSERT_NE(BitIt, result.valueBits.end());

  llvm::APInt liveIn =
      unionFactForBlock(result.blockFacts, &Id->getEntryBlock());
  EXPECT_TRUE(liveIn[BitIt->second]);
}

TEST(NPAInterproceduralClients,
     BackwardEngineSeparatesExactSummarySolveFromPropagationLimitOnRecursion) {
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

  RecursivePropagationLimitedBackwardAnalysis analysis;
  auto result = npa::BackwardInterEngine<
      RecursiveSummaryDomain, RecursivePropagationLimitedBackwardAnalysis>::
      run(*module, analysis, false, npa::LinearStrategy::SCC);

  auto *Recur = module->getFunction("recur");
  ASSERT_NE(Recur, nullptr);
  EXPECT_TRUE(result.status.summary_solve.converged);
  EXPECT_FALSE(result.status.summary_solve.hit_limit);
  EXPECT_FALSE(result.status.propagation_converged);
  EXPECT_TRUE(result.status.propagation_hit_limit);
  EXPECT_FALSE(result.status.overall_converged);
  EXPECT_TRUE(result.status.approximated);
  EXPECT_NE(result.summaries.find(npa::FunctionKey{Recur}),
            result.summaries.end());
}

TEST(NPAInterproceduralClients, RecursiveIntervalAnalysisConverges) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define i32 @g(i32 %x) {
    entry:
      %cmp = icmp sle i32 %x, 0
      br i1 %cmp, label %base, label %step

    base:
      ret i32 %x

    step:
      %dec = sub i32 %x, 1
      %r = call i32 @f(i32 %dec)
      ret i32 %r
    }

    define i32 @f(i32 %x) {
    entry:
      %cmp = icmp sle i32 %x, 0
      br i1 %cmp, label %base, label %step

    base:
      ret i32 %x

    step:
      %dec = sub i32 %x, 1
      %r = call i32 @g(i32 %dec)
      ret i32 %r
    }

    define i32 @main() {
    entry:
      %r = call i32 @f(i32 3)
      ret i32 %r
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *Arg = &*F->arg_begin();

  auto result = npa::InterIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, &F->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  EXPECT_TRUE(states.front()->reachable);
  auto It = states.front()->values.find(Arg);
  if (It != states.front()->values.end())
    EXPECT_TRUE(It->second.hasLower || It->second.hasUpper);
}

TEST(NPAInterproceduralClients, IntervalAnalysisReportsFactWidening) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @grow(i32 %x) {
    entry:
      %cmp = icmp slt i32 %x, 10
      br i1 %cmp, label %step, label %exit

    step:
      %next = add i32 %x, 1
      call void @grow(i32 %next)
      ret void

    exit:
      ret void
    }

    define void @main() {
    entry:
      call void @grow(i32 0)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto result = npa::InterIntervalAnalysis::run(*module);

  EXPECT_TRUE(result.status.summary_solve.converged);
  EXPECT_TRUE(result.status.used_fact_widening);
  EXPECT_TRUE(result.status.approximated);
  EXPECT_FALSE(result.status.overall_converged);
}

TEST(NPAInterproceduralClients, IntervalCastKeepsZextTrueAsOne) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %v = zext i1 true to i32
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Value = findInstructionByName(*Main, "v");
  ASSERT_NE(Value, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Value);
  ASSERT_NE(It, states.front()->values.end());
  expectIntervalPoint(It->second, unsignedAPInt(32, 1),
                      npa::IntervalOrdering::Unsigned);
}

TEST(NPAInterproceduralClients, IntervalSignedDivisionUsesAllEndpointPairs) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %c1, i1 %c2) {
    entry:
      %x = select i1 %c1, i32 -10, i32 5
      %y = select i1 %c2, i32 -2, i32 -1
      %q = sdiv i32 %x, %y
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Quotient = findInstructionByName(*Main, "q");
  ASSERT_NE(Quotient, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Quotient);
  ASSERT_NE(It, states.front()->values.end());
  expectIntervalRange(It->second, signedAPInt(32, -5), signedAPInt(32, 10),
                      npa::IntervalOrdering::Signed);
}

TEST(NPAInterproceduralClients, IntervalUnsignedOpsUseUnsignedSemantics) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %cmp = icmp ugt i32 -1, 1
      %q = udiv i32 -1, 2
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Cmp = findInstructionByName(*Main, "cmp");
  auto *Quotient = findInstructionByName(*Main, "q");
  ASSERT_NE(Cmp, nullptr);
  ASSERT_NE(Quotient, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto CmpIt = states.front()->values.find(Cmp);
  ASSERT_NE(CmpIt, states.front()->values.end());
  expectIntervalPoint(CmpIt->second, unsignedAPInt(1, 1),
                      npa::IntervalOrdering::Signed);

  auto QuotientIt = states.front()->values.find(Quotient);
  ASSERT_NE(QuotientIt, states.front()->values.end());
  expectIntervalPoint(QuotientIt->second, unsignedAPInt(32, 2147483647),
                      npa::IntervalOrdering::Unsigned);
}

TEST(NPAInterproceduralClients, IntervalSupportsLargeUnsignedValues) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %q = udiv i64 -1, 1
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Q = findInstructionByName(*Main, "q");
  ASSERT_NE(Q, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Q);
  ASSERT_NE(It, states.front()->values.end());
  expectIntervalPoint(It->second,
                      unsignedAPInt(64, std::numeric_limits<uint64_t>::max()),
                      npa::IntervalOrdering::Unsigned);
}

TEST(NPAInterproceduralClients, IntervalPreservesPhiConditionCorrelation) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right

    left:
      br label %merge

    right:
      br label %merge

    merge:
      %x = phi i32 [ 1, %left ], [ 2, %right ]
      %c = zext i1 %cond to i32
      %y = add i32 %x, %c
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Y = findInstructionByName(*Main, "y");
  ASSERT_NE(Y, nullptr);
  auto NextIt = std::next(Main->begin(), 4);
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front()->values.find(Y);
  ASSERT_NE(It, states.front()->values.end());
  expectIntervalPoint(It->second, signedAPInt(32, 2),
                      npa::IntervalOrdering::Signed);
}

TEST(NPAInterproceduralClients,
     IntervalDefaultSwitchNarrowsRepresentableRange) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %c1, i1 %c2) {
    entry:
      %x = select i1 %c1, i32 0, i32 2
      %y = select i1 %c2, i32 %x, i32 1
      switch i32 %y, label %default [ i32 0, label %case0 ]

    case0:
      br label %join

    default:
      br label %join

    join:
      %z = phi i32 [ 0, %case0 ], [ %y, %default ]
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto DefaultIt = std::next(Main->begin(), 2);
  ASSERT_NE(DefaultIt, Main->end());
  auto *Default = &*DefaultIt;

  auto result = npa::InterIntervalAnalysis::run(*module);
  auto states = statesForBlock(result.blockFacts, Default);
  ASSERT_EQ(states.size(), 1u);

  auto *Y = findInstructionByName(*Main, "y");
  ASSERT_NE(Y, nullptr);
  auto It = states.front()->values.find(Y);
  ASSERT_NE(It, states.front()->values.end());
  expectIntervalRange(It->second, signedAPInt(32, 1), signedAPInt(32, 2),
                      npa::IntervalOrdering::Signed);
}

namespace {

using TraceTransferDomain = npa::PathTransferSummary<char>;

struct BackwardTensorLangDomain {
  using value_type = std::set<std::string>;
  using test_type = bool;
  static constexpr bool idempotent = true;
  static constexpr bool commutative_extend = false;
  static constexpr std::size_t MaxLen = 8;

  static value_type zero() { return {}; }
  static value_type one() { return {""}; }
  static bool equal(const value_type &a, const value_type &b) { return a == b; }
  static value_type combine(const value_type &a, const value_type &b) {
    value_type out = a;
    out.insert(b.begin(), b.end());
    return out;
  }
  static value_type ndetCombine(const value_type &a, const value_type &b) {
    return combine(a, b);
  }
  static value_type condCombine(bool phi, const value_type &t,
                                const value_type &e) {
    return phi ? t : e;
  }
  static value_type extend(const value_type &a, const value_type &b) {
    value_type out;
    for (const auto &lhs : a) {
      for (const auto &rhs : b) {
        std::string s = lhs + rhs;
        if (s.size() <= MaxLen)
          out.insert(std::move(s));
      }
    }
    return out;
  }
  static value_type extend_lin(const value_type &a, const value_type &b) {
    return extend(a, b);
  }
  static value_type subtract(const value_type &a, const value_type &) {
    return a;
  }
};

class BackwardTensorAnalysis {
public:
  using FactType = BackwardTensorLangDomain::value_type;
  using E = npa::E0<BackwardTensorLangDomain>;

  FactType getExitValue(const llvm::Function &) const {
    return BackwardTensorLangDomain::one();
  }

  E getTransfer(llvm::Instruction &, E current) const { return current; }

  FactType getCallReturnTransfer(const llvm::CallBase &,
                                 const llvm::Function &) const {
    return {std::string("r")};
  }

  FactType getCallEntryTransfer(const llvm::CallBase &,
                                const llvm::Function &) const {
    return {std::string("e")};
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return BackwardTensorLangDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return BackwardTensorLangDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return BackwardTensorLangDomain::equal(lhs, rhs);
  }
};

class BackwardEdgeTransferAnalysis {
public:
  using FactType = TraceTransferDomain::value_type;
  using E = npa::E0<TraceTransferDomain>;

  FactType getExitValue(const llvm::Function &) const {
    return TraceTransferDomain::one();
  }

  E getTransfer(llvm::Instruction &, E current) const { return current; }

  FactType getEdgeTransfer(const llvm::Instruction &term,
                           const llvm::BasicBlock &succ) const {
    auto *Branch = llvm::dyn_cast<llvm::BranchInst>(&term);
    if (!Branch || !Branch->isConditional())
      return TraceTransferDomain::one();
    return Branch->getSuccessor(0) == &succ
               ? TraceTransferDomain::singleton('T')
               : TraceTransferDomain::singleton('F');
  }

  FactType applySummary(const FactType &summary, const FactType &fact) const {
    return TraceTransferDomain::extend(summary, fact);
  }

  FactType joinFacts(const FactType &lhs, const FactType &rhs) const {
    return TraceTransferDomain::combine(lhs, rhs);
  }

  bool factsEqual(const FactType &lhs, const FactType &rhs) const {
    return TraceTransferDomain::equal(lhs, rhs);
  }
};

} // namespace

namespace npa {
template <> struct TensorSemiringTraits<BackwardTensorLangDomain> {
  using tensor_domain = ExactTensorProductLift<BackwardTensorLangDomain>;

  static bool available() { return true; }
  static bool paper_admissible() { return false; }

  static tensor_domain::value_type
  right_constant(const BackwardTensorLangDomain::value_type &v) {
    return domain_equal<BackwardTensorLangDomain>(
               v, BackwardTensorLangDomain::zero())
               ? tensor_domain::zero()
               : tensor_domain::singleton(BackwardTensorLangDomain::one(), v);
  }

  static tensor_domain::value_type
  left_constant(const BackwardTensorLangDomain::value_type &v) {
    return domain_equal<BackwardTensorLangDomain>(
               v, BackwardTensorLangDomain::zero())
               ? tensor_domain::zero()
               : tensor_domain::singleton(v, BackwardTensorLangDomain::one());
  }

  static tensor_domain::value_type
  constant(const BackwardTensorLangDomain::value_type &v) {
    return right_constant(v);
  }

  static tensor_domain::value_type
  couple(const BackwardTensorLangDomain::value_type &lhs,
         const BackwardTensorLangDomain::value_type &rhs) {
    return tensor_domain::singleton(lhs, rhs);
  }

  static BackwardTensorLangDomain::value_type
  readout(const tensor_domain::value_type &v) {
    return tensor_domain::project(v);
  }
};
} // namespace npa

namespace {

TEST(NPAInterproceduralClients, BackwardEngineAppliesEdgeTransfers) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right

    left:
      ret void

    right:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);

  BackwardEdgeTransferAnalysis analysis;
  auto result = npa::BackwardInterEngine<
      TraceTransferDomain, BackwardEdgeTransferAnalysis>::run(*module,
                                                              analysis);

  auto Facts = statesForBlock(result.blockEntryFacts, &Main->getEntryBlock());
  ASSERT_EQ(Facts.size(), 1u);
  EXPECT_TRUE(containsPath(*Facts.front(), {'T'}));
  EXPECT_TRUE(containsPath(*Facts.front(), {'F'}));
}
