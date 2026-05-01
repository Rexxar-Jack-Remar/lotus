TEST(NPAInterproceduralClients,
     BackwardTensorStrategyFallsBackCleanlyForLeftLinearInterproceduralCalls) {
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

  BackwardTensorAnalysis analysis;
  testing::internal::CaptureStderr();
  auto result = npa::BackwardInterEngine<
      BackwardTensorLangDomain,
      BackwardTensorAnalysis>::run(*module, analysis, true,
                                   npa::LinearStrategy::TensorProduct);
  std::string stderrOutput = testing::internal::GetCapturedStderr();

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto Facts = statesForBlock(result.blockEntryFacts, &Main->getEntryBlock());
  ASSERT_EQ(Facts.size(), 1u);
  EXPECT_TRUE(Facts.front()->count("er"));
  EXPECT_NE(stderrOutput.find("already left-linear"), std::string::npos);
}

TEST(NPAInterproceduralClients, ProgramTransferDomainPreservesLongPaths) {
  using Domain = npa::ProgramTransferDomain<char>;

  Domain::value_type path = Domain::one();
  for (int i = 0; i < 300; ++i)
    path = Domain::extend(Domain::singleton('a'), path);

  std::vector<char> expected(300, 'a');
  EXPECT_EQ(path.paths.size(), 1u);
  EXPECT_EQ(path.paths.count(expected), 1u);
}


TEST(NPAInterproceduralClients, IntervalCompareAndSelectUseForcedRanges) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %c) {
    entry:
      %x = select i1 %c, i32 1, i32 2
      %y = select i1 %c, i32 5, i32 6
      %cmp = icmp slt i32 %x, %y
      %sel = select i1 %cmp, i32 9, i32 10
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Cmp = findInstructionByName(*Main, "cmp");
  auto *Sel = findInstructionByName(*Main, "sel");
  ASSERT_NE(Cmp, nullptr);
  ASSERT_NE(Sel, nullptr);
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

  auto SelIt = states.front()->values.find(Sel);
  ASSERT_NE(SelIt, states.front()->values.end());
  expectIntervalPoint(SelIt->second, signedAPInt(32, 9),
                      npa::IntervalOrdering::Signed);
}

} // namespace
