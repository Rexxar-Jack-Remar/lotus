#include "InterAffineEqualitiesTestSupport.h"

TEST(InterAffineEqualities, BitwiseAndMaskKeepsLowBitCongruence) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %y = and i32 %x, 15
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = &*Main->arg_begin();
  auto *Y = findInstructionByName(*Main, "y");
  ASSERT_NE(Y, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  const int64_t Mod16Scale = congruenceScale(4);
  EXPECT_TRUE(
      stateHasEquality(states.front(), 0, {{Y, Mod16Scale}, {X, -Mod16Scale}}));
}
TEST(InterAffineEqualities, BitwiseSelfOperationsStayPrecise) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %anded = and i32 %x, %x
      %ored = or i32 %x, %x
      %xored = xor i32 %x, %x
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = &*Main->arg_begin();
  auto *Anded = findInstructionByName(*Main, "anded");
  auto *Ored = findInstructionByName(*Main, "ored");
  auto *Xored = findInstructionByName(*Main, "xored");
  ASSERT_NE(Anded, nullptr);
  ASSERT_NE(Ored, nullptr);
  ASSERT_NE(Xored, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto AndIt = states.front().values.find(Anded);
  ASSERT_NE(AndIt, states.front().values.end());
  EXPECT_FALSE(AndIt->second.top);
  EXPECT_EQ(AndIt->second.constant, 0);
  ASSERT_EQ(AndIt->second.terms.size(), 1u);
  EXPECT_EQ(AndIt->second.terms.at(X), 1);

  auto OrIt = states.front().values.find(Ored);
  ASSERT_NE(OrIt, states.front().values.end());
  EXPECT_FALSE(OrIt->second.top);
  EXPECT_EQ(OrIt->second.constant, 0);
  ASSERT_EQ(OrIt->second.terms.size(), 1u);
  EXPECT_EQ(OrIt->second.terms.at(X), 1);

  auto XorIt = states.front().values.find(Xored);
  ASSERT_NE(XorIt, states.front().values.end());
  EXPECT_FALSE(XorIt->second.top);
  EXPECT_TRUE(XorIt->second.terms.empty());
  EXPECT_EQ(XorIt->second.constant, 0);
}
TEST(InterAffineEqualities, BitwiseAndClearedMaskKeepsZeroCongruence) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %y = and i32 %x, -4
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
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  const int64_t Mod4Scale = congruenceScale(2);
  EXPECT_TRUE(stateHasEquality(states.front(), 0, {{Y, Mod4Scale}}));
}
TEST(InterAffineEqualities, PowerOfTwoRemainderKeepsLowBitCongruence) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %y = urem i32 %x, 8
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = &*Main->arg_begin();
  auto *Y = findInstructionByName(*Main, "y");
  ASSERT_NE(Y, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  const int64_t Mod8Scale = congruenceScale(3);
  EXPECT_TRUE(
      stateHasEquality(states.front(), 0, {{Y, Mod8Scale}, {X, -Mod8Scale}}));
}
TEST(InterAffineEqualities, CompositeUnsignedRemainderKeepsCommonLowBits) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %y = urem i32 %x, 12
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = &*Main->arg_begin();
  auto *Y = findInstructionByName(*Main, "y");
  ASSERT_NE(Y, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  const int64_t Mod4Scale = congruenceScale(2);
  EXPECT_TRUE(
      stateHasEquality(states.front(), 0, {{Y, Mod4Scale}, {X, -Mod4Scale}}));
}
TEST(InterAffineEqualities, CompositeSignedRemainderKeepsCommonLowBits) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %y = srem i32 %x, -12
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = &*Main->arg_begin();
  auto *Y = findInstructionByName(*Main, "y");
  ASSERT_NE(Y, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  const int64_t Mod4Scale = congruenceScale(2);
  EXPECT_TRUE(
      stateHasEquality(states.front(), 0, {{Y, Mod4Scale}, {X, -Mod4Scale}}));
}
TEST(InterAffineEqualities, BitwiseOrUsesPartialConstantMiddleZeros) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x, i32 %y) {
    entry:
      %shifted = shl i32 %x, 4
      %z = or i32 %shifted, %y
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *ArgIt = Main->arg_begin();
  ++ArgIt;
  auto *Y = &*ArgIt;
  auto *Z = findInstructionByName(*Main, "z");
  ASSERT_NE(Z, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  const int64_t Mod16Scale = congruenceScale(4);
  EXPECT_TRUE(
      stateHasEquality(states.front(), 0, {{Z, Mod16Scale}, {Y, -Mod16Scale}}));
}
TEST(InterAffineEqualities, BitwiseAndUsesPartialConstantMiddleOnes) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x, i32 %y) {
    entry:
      %shifted = shl i32 %x, 4
      %lhs = add i32 %shifted, 15
      %z = and i32 %lhs, %y
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *ArgIt = Main->arg_begin();
  ++ArgIt;
  auto *Y = &*ArgIt;
  auto *Z = findInstructionByName(*Main, "z");
  ASSERT_NE(Z, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  const int64_t Mod16Scale = congruenceScale(4);
  EXPECT_TRUE(
      stateHasEquality(states.front(), 0, {{Z, Mod16Scale}, {Y, -Mod16Scale}}));
}
TEST(InterAffineEqualities, BitwiseXorUsesPartialConstantMiddleOnesComplement) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x, i32 %y) {
    entry:
      %shifted = shl i32 %x, 4
      %lhs = add i32 %shifted, 15
      %z = xor i32 %lhs, %y
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *ArgIt = Main->arg_begin();
  ++ArgIt;
  auto *Y = &*ArgIt;
  auto *Z = findInstructionByName(*Main, "z");
  ASSERT_NE(Z, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  const int64_t Mod16Scale = congruenceScale(4);
  EXPECT_TRUE(stateHasEquality(states.front(), -Mod16Scale,
                               {{Z, Mod16Scale}, {Y, Mod16Scale}}));
}
