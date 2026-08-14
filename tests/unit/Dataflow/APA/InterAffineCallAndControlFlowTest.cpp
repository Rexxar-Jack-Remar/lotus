#include "InterAffineEqualitiesTestSupport.h"

TEST(InterAffineEqualities, TransferSymbolicRelationsAcrossCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @sink(i32 %x, i32 %y) {
    entry:
      ret void
    }

    define void @caller(i32 %a) {
    entry:
      %b = add i32 %a, 4
      call void @sink(i32 %a, i32 %b)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Sink = module->getFunction("sink");
  auto *Caller = module->getFunction("caller");
  ASSERT_NE(Sink, nullptr);
  ASSERT_NE(Caller, nullptr);
  auto *X = &*Sink->arg_begin();
  auto *YIt = Sink->arg_begin();
  ++YIt;
  auto *Y = &*YIt;
  auto *A = &*Caller->arg_begin();

  auto result = elimination::InterAffineEqualities::run(*module);
  auto relations =
      relationsForBlock(result.blockRelations, &Sink->getEntryBlock());
  ASSERT_EQ(relations.size(), 1u);
  EXPECT_FALSE(relations.front()->bottom);
  auto states = materializedAffineStatesForBlock(result.blockRelations,
                                                 &Sink->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);
  auto XIt = states.front().values.find(X);
  auto YValueIt = states.front().values.find(Y);
  ASSERT_NE(XIt, states.front().values.end());
  ASSERT_NE(YValueIt, states.front().values.end());

  EXPECT_FALSE(XIt->second.top);
  EXPECT_EQ(XIt->second.constant, 0);
  ASSERT_EQ(XIt->second.terms.size(), 1u);
  auto XCoeffIt = XIt->second.terms.find(A);
  ASSERT_NE(XCoeffIt, XIt->second.terms.end());
  EXPECT_EQ(XCoeffIt->second, 1);

  EXPECT_FALSE(YValueIt->second.top);
  EXPECT_EQ(YValueIt->second.constant, 4);
  EXPECT_EQ(YValueIt->second.terms.size(), 1u);
  auto YCoeffIt = YValueIt->second.terms.find(A);
  ASSERT_NE(YCoeffIt, YValueIt->second.terms.end());
  EXPECT_EQ(YCoeffIt->second, 1);
}
TEST(InterAffineEqualities, PreservesCallerLocalEffectAcrossResolvedCall) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @sink(i32 %x) {
    entry:
      %tmp = add i32 %x, 9
      ret void
    }

    define void @caller(i32 %a) {
    entry:
      %l = add i32 %a, 2
      call void @sink(i32 %a)
      %after = add i32 %l, 1
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Caller = module->getFunction("caller");
  ASSERT_NE(Caller, nullptr);
  auto *A = &*Caller->arg_begin();
  auto *After = findInstructionByName(*Caller, "after");
  ASSERT_NE(After, nullptr);
  auto NextIt = std::next(Caller->begin());
  ASSERT_NE(NextIt, Caller->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto AfterIt = states.front().values.find(After);
  ASSERT_NE(AfterIt, states.front().values.end());
  EXPECT_FALSE(AfterIt->second.top);
  EXPECT_EQ(AfterIt->second.constant, 3);
  ASSERT_EQ(AfterIt->second.terms.size(), 1u);
  EXPECT_EQ(AfterIt->second.terms.at(A), 1);
}
TEST(InterAffineEqualities, ExposesEqualityBasisRows) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @sink(i32 %x, i32 %y) {
    entry:
      ret void
    }

    define void @caller(i32 %a) {
    entry:
      %b = add i32 %a, 4
      call void @sink(i32 %a, i32 %b)
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Sink = module->getFunction("sink");
  auto *Caller = module->getFunction("caller");
  ASSERT_NE(Sink, nullptr);
  ASSERT_NE(Caller, nullptr);
  auto *X = &*Sink->arg_begin();
  auto *YIt = Sink->arg_begin();
  ++YIt;
  auto *Y = &*YIt;
  auto *A = &*Caller->arg_begin();

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations,
                                                 &Sink->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);

  bool foundXEqA = false;
  bool foundYEqAPlus4 = false;
  for (const auto &equality : states.front().equalities) {
    foundXEqA = foundXEqA ||
                equalityMatchesUpToNegation(equality, 0, {{X, 1}, {A, -1}});
    foundYEqAPlus4 = foundYEqAPlus4 || equalityMatchesUpToNegation(
                                           equality, 4, {{Y, 1}, {A, -1}});
  }
  EXPECT_TRUE(foundXEqA);
  EXPECT_TRUE(foundYEqAPlus4);
}
TEST(InterAffineEqualities, DefaultSwitchRemainsUnrefinedByDisequality) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      switch i32 %x, label %default [ i32 0, label %case0 ]

    case0:
      br label %join

    default:
      br label %join

    join:
      %y = phi i32 [ 0, %case0 ], [ %x, %default ]
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

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states =
      materializedAffineStatesForBlock(result.blockRelations, Default);
  ASSERT_EQ(states.size(), 1u);

  auto *X = &*Main->arg_begin();
  auto It = states.front().values.find(X);
  ASSERT_NE(It, states.front().values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 0);
  ASSERT_EQ(It->second.terms.size(), 1u);
  auto XIt = It->second.terms.find(X);
  ASSERT_NE(XIt, It->second.terms.end());
  EXPECT_EQ(XIt->second, 1);
}
TEST(InterAffineEqualities, CastAndSelectUseKnownConditionValue) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %cond = trunc i32 1 to i1
      %x = select i1 %cond, i32 4, i32 7
      %v = zext i1 %cond to i32
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = findInstructionByName(*Main, "x");
  auto *V = findInstructionByName(*Main, "v");
  ASSERT_NE(X, nullptr);
  ASSERT_NE(V, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto XIt = states.front().values.find(X);
  ASSERT_NE(XIt, states.front().values.end());
  EXPECT_FALSE(XIt->second.top);
  EXPECT_TRUE(XIt->second.terms.empty());
  EXPECT_EQ(XIt->second.constant, 4);

  auto VIt = states.front().values.find(V);
  ASSERT_NE(VIt, states.front().values.end());
  EXPECT_FALSE(VIt->second.top);
  EXPECT_TRUE(VIt->second.terms.empty());
  EXPECT_EQ(VIt->second.constant, 1);
}
TEST(InterAffineEqualities, PhiKeepsBranchConditionAtMerge) {
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
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Cond = &*Main->arg_begin();
  auto MergeIt = std::next(Main->begin(), 3);
  ASSERT_NE(MergeIt, Main->end());
  auto *Merge = &*MergeIt;
  auto *X = findInstructionByName(*Main, "x");
  ASSERT_NE(X, nullptr);

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Merge);
  ASSERT_EQ(states.size(), 1u);

  auto XIt = states.front().values.find(X);
  ASSERT_NE(XIt, states.front().values.end());
  EXPECT_FALSE(XIt->second.top);
  EXPECT_EQ(XIt->second.constant, 2);
  ASSERT_EQ(XIt->second.terms.size(), 1u);
  auto CondIt = XIt->second.terms.find(Cond);
  ASSERT_NE(CondIt, XIt->second.terms.end());
  EXPECT_EQ(CondIt->second, -1);
}
TEST(InterAffineEqualities, SwitchOnAffineConstantRoutesToTakenCase) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %y = add i32 7, 1
      switch i32 %y, label %default [ i32 8, label %case8 ]

    case8:
      br label %exit

    default:
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto CaseIt = std::next(Main->begin());
  ASSERT_NE(CaseIt, Main->end());
  auto *Case8 = &*CaseIt;
  auto DefaultIt = std::next(Main->begin(), 2);
  ASSERT_NE(DefaultIt, Main->end());
  auto *Default = &*DefaultIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto caseRelations = relationsForBlock(result.blockRelations, Case8);
  ASSERT_EQ(caseRelations.size(), 1u);
  EXPECT_FALSE(caseRelations.front()->bottom);

  auto defaultRelations = relationsForBlock(result.blockRelations, Default);
  ASSERT_EQ(defaultRelations.size(), 1u);
  EXPECT_TRUE(defaultRelations.front()->bottom);
}
