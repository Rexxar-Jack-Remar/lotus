#include "InterAffineEqualitiesTestSupport.h"

TEST(InterAffineEqualities, CompareOfSameValueProducesConstant) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %cmp = icmp eq i32 %x, %x
      %sel = select i1 %cmp, i32 11, i32 12
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

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto CmpIt = states.front().values.find(Cmp);
  ASSERT_NE(CmpIt, states.front().values.end());
  EXPECT_FALSE(CmpIt->second.top);
  EXPECT_TRUE(CmpIt->second.terms.empty());
  EXPECT_EQ(CmpIt->second.constant, 1);

  auto SelIt = states.front().values.find(Sel);
  ASSERT_NE(SelIt, states.front().values.end());
  EXPECT_FALSE(SelIt->second.top);
  EXPECT_TRUE(SelIt->second.terms.empty());
  EXPECT_EQ(SelIt->second.constant, 11);
}
TEST(InterAffineEqualities,
     CompareEquivalentAffineExpressionsProducesConstant) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %a = add i32 %x, 1
      %b = add i32 1, %x
      %cmp = icmp eq i32 %a, %b
      br i1 %cmp, label %taken, label %other

    taken:
      br label %exit

    other:
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto TakenIt = std::next(Main->begin());
  ASSERT_NE(TakenIt, Main->end());
  auto *Taken = &*TakenIt;
  auto OtherIt = std::next(Main->begin(), 2);
  ASSERT_NE(OtherIt, Main->end());
  auto *Other = &*OtherIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto takenStates =
      materializedAffineStatesForBlock(result.blockRelations, Taken);
  ASSERT_EQ(takenStates.size(), 1u);
  EXPECT_TRUE(takenStates.front().reachable);

  auto otherRelations = relationsForBlock(result.blockRelations, Other);
  ASSERT_EQ(otherRelations.size(), 1u);
  EXPECT_TRUE(otherRelations.front()->bottom);
}
TEST(InterAffineEqualities, TrueEqualityBranchRefinesComparedValue) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %cmp = icmp eq i32 %x, 7
      br i1 %cmp, label %equal, label %other

    equal:
      br label %exit

    other:
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = &*Main->arg_begin();
  auto EqualIt = std::next(Main->begin());
  ASSERT_NE(EqualIt, Main->end());
  auto *Equal = &*EqualIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Equal);
  ASSERT_EQ(states.size(), 1u);

  EXPECT_TRUE(stateHasEquality(states.front(), 7, {{X, 1}}));
}
TEST(InterAffineEqualities, AssumeLikeCallRefinesComparedValue) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare void @__VERIFIER_assume(i1)

    define void @main(i32 %x) {
    entry:
      %cmp = icmp eq i32 %x, 7
      call void @__VERIFIER_assume(i1 %cmp)
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = &*Main->arg_begin();
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  EXPECT_TRUE(stateHasEquality(states.front(), 7, {{X, 1}}));
}
TEST(InterAffineEqualities, AssumeLikeCallWithFalseConditionIsBottom) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    declare void @__VERIFIER_assume(i1)

    define void @main() {
    entry:
      call void @__VERIFIER_assume(i1 false)
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto relations = relationsForBlock(result.blockRelations, Next);
  ASSERT_EQ(relations.size(), 1u);
  EXPECT_TRUE(elimination::AffineRelationDomain::isBottom(*relations.front()));
}
TEST(InterAffineEqualities,
     FalseInequalityBranchRefinesComparedVariablesEqual) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x, i32 %y) {
    entry:
      %cmp = icmp ne i32 %x, %y
      br i1 %cmp, label %different, label %equal

    different:
      br label %exit

    equal:
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *ArgIt = Main->arg_begin();
  auto *X = &*ArgIt;
  ++ArgIt;
  auto *Y = &*ArgIt;
  auto EqualIt = std::next(Main->begin(), 2);
  ASSERT_NE(EqualIt, Main->end());
  auto *Equal = &*EqualIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Equal);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front().values.find(X);
  ASSERT_NE(It, states.front().values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 0);
  ASSERT_EQ(It->second.terms.size(), 1u);
  auto YIt = It->second.terms.find(Y);
  ASSERT_NE(YIt, It->second.terms.end());
  EXPECT_EQ(YIt->second, 1);
}
TEST(InterAffineEqualities, SingletonUnsignedComparisonRefinesToConstant) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %cmp = icmp ult i32 %x, 1
      br i1 %cmp, label %zero, label %other

    zero:
      br label %exit

    other:
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = &*Main->arg_begin();
  auto ZeroIt = std::next(Main->begin());
  ASSERT_NE(ZeroIt, Main->end());
  auto *Zero = &*ZeroIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Zero);
  ASSERT_EQ(states.size(), 1u);

  EXPECT_TRUE(stateHasEquality(states.front(), 0, {{X, 1}}));
}
TEST(InterAffineEqualities, FalseUnsignedComparisonRefinesToZeroConstant) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %cmp = icmp ugt i32 %x, 0
      br i1 %cmp, label %nonzero, label %zero

    nonzero:
      br label %exit

    zero:
      br label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = &*Main->arg_begin();
  auto ZeroIt = std::next(Main->begin(), 2);
  ASSERT_NE(ZeroIt, Main->end());
  auto *Zero = &*ZeroIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Zero);
  ASSERT_EQ(states.size(), 1u);

  EXPECT_TRUE(stateHasEquality(states.front(), 0, {{X, 1}}));
}
TEST(InterAffineEqualities, ExtremeComparisonProducesConstantResult) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %always = icmp uge i32 %x, 0
      %never = icmp ult i32 %x, 0
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Always = findInstructionByName(*Main, "always");
  auto *Never = findInstructionByName(*Main, "never");
  ASSERT_NE(Always, nullptr);
  ASSERT_NE(Never, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto AlwaysIt = states.front().values.find(Always);
  ASSERT_NE(AlwaysIt, states.front().values.end());
  EXPECT_FALSE(AlwaysIt->second.top);
  EXPECT_TRUE(AlwaysIt->second.terms.empty());
  EXPECT_EQ(AlwaysIt->second.constant, 1);

  auto NeverIt = states.front().values.find(Never);
  ASSERT_NE(NeverIt, states.front().values.end());
  EXPECT_FALSE(NeverIt->second.top);
  EXPECT_TRUE(NeverIt->second.terms.empty());
  EXPECT_EQ(NeverIt->second.constant, 0);
}
