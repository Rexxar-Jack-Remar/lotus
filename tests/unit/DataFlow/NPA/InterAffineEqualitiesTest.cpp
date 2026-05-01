#include "Dataflow/NPA/Analyses/Inter/InterAffineEqualities.h"

#include "TestUtils/LLVMHelpers.h"

#include <iterator>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

namespace {

using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModule;

std::vector<npa::AffineState> materializedAffineStatesForBlock(
    const std::map<npa::BlockKey, npa::AffineRelationDomain::value_type> &facts,
    const llvm::BasicBlock *block) {
  std::vector<npa::AffineState> out;
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    out.push_back(npa::materializeAffineExpressions(entry.second));
  }
  return out;
}

std::vector<const npa::AffineRelationDomain::value_type *> relationsForBlock(
    const std::map<npa::BlockKey, npa::AffineRelationDomain::value_type> &facts,
    const llvm::BasicBlock *block) {
  std::vector<const npa::AffineRelationDomain::value_type *> out;
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    out.push_back(&entry.second);
  }
  return out;
}

bool equalityMatchesUpToNegation(
    const npa::AffineEquality &equality, int64_t constant,
    std::initializer_list<std::pair<const llvm::Value *, int64_t>> terms) {
  auto matches = [&](int sign) {
    if (equality.constant != sign * constant)
      return false;
    if (equality.terms.size() != terms.size())
      return false;
    for (const auto &term : terms) {
      auto It = equality.terms.find(term.first);
      if (It == equality.terms.end() || It->second != sign * term.second)
        return false;
    }
    return true;
  };
  return matches(1) || matches(-1);
}

} // namespace

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

  auto result = npa::InterAffineEqualities::run(*module);
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

  auto result = npa::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations,
                                                 &Sink->getEntryBlock());
  ASSERT_EQ(states.size(), 1u);

  bool foundXEqA = false;
  bool foundYEqAPlus4 = false;
  for (const auto &equality : states.front().equalities) {
    foundXEqA = foundXEqA ||
                equalityMatchesUpToNegation(equality, 0, {{X, 1}, {A, -1}});
    foundYEqAPlus4 =
        foundYEqAPlus4 ||
        equalityMatchesUpToNegation(equality, 4, {{Y, 1}, {A, -1}});
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

  auto result = npa::InterAffineEqualities::run(*module);
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

  auto result = npa::InterAffineEqualities::run(*module);
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

  auto result = npa::InterAffineEqualities::run(*module);
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

TEST(InterAffineEqualities, CompareEquivalentAffineExpressionsProducesConstant) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %a = add i32 %x, 1
      %b = add i32 1, %x
      %cmp = icmp eq i32 %a, %b
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Cmp = findInstructionByName(*Main, "cmp");
  ASSERT_NE(Cmp, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto CmpIt = states.front().values.find(Cmp);
  ASSERT_NE(CmpIt, states.front().values.end());
  EXPECT_FALSE(CmpIt->second.top);
  EXPECT_TRUE(CmpIt->second.terms.empty());
  EXPECT_EQ(CmpIt->second.constant, 1);
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

  auto result = npa::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Equal);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front().values.find(X);
  ASSERT_NE(It, states.front().values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_TRUE(It->second.terms.empty());
  EXPECT_EQ(It->second.constant, 7);
}

TEST(InterAffineEqualities, FalseInequalityBranchRefinesComparedVariablesEqual) {
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

  auto result = npa::InterAffineEqualities::run(*module);
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

  auto result = npa::InterAffineEqualities::run(*module);
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

TEST(InterAffineEqualities, TracksModularWrapForConstants) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main() {
    entry:
      %y = add i8 127, 1
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

  auto result = npa::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front().values.find(Y);
  ASSERT_NE(It, states.front().values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_TRUE(It->second.terms.empty());
  EXPECT_EQ(It->second.constant, -128);
}

TEST(InterAffineEqualities, ZextOfBooleanArgumentStaysSymbolic) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %b) {
    entry:
      %v = zext i1 %b to i32
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Arg = &*Main->arg_begin();
  auto *V = findInstructionByName(*Main, "v");
  ASSERT_NE(V, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front().values.find(V);
  ASSERT_NE(It, states.front().values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 0);
  ASSERT_EQ(It->second.terms.size(), 1u);
  auto CoeffIt = It->second.terms.find(Arg);
  ASSERT_NE(CoeffIt, It->second.terms.end());
  EXPECT_EQ(CoeffIt->second, 1);
}

TEST(InterAffineEqualities, SextOfBooleanArgumentUsesSignSemantics) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i1 %b) {
    entry:
      %v = sext i1 %b to i32
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Arg = &*Main->arg_begin();
  auto *V = findInstructionByName(*Main, "v");
  ASSERT_NE(V, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front().values.find(V);
  ASSERT_NE(It, states.front().values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 0);
  ASSERT_EQ(It->second.terms.size(), 1u);
  auto CoeffIt = It->second.terms.find(Arg);
  ASSERT_NE(CoeffIt, It->second.terms.end());
  EXPECT_EQ(CoeffIt->second, -1);
}

TEST(InterAffineEqualities, ShiftAndNegationStayAffine) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %neg = sub i32 0, %x
      %dbl = shl i32 %neg, 1
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Arg = &*Main->arg_begin();
  auto *Neg = findInstructionByName(*Main, "neg");
  auto *Dbl = findInstructionByName(*Main, "dbl");
  ASSERT_NE(Neg, nullptr);
  ASSERT_NE(Dbl, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = npa::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto NegIt = states.front().values.find(Neg);
  ASSERT_NE(NegIt, states.front().values.end());
  EXPECT_FALSE(NegIt->second.top);
  EXPECT_EQ(NegIt->second.constant, 0);
  ASSERT_EQ(NegIt->second.terms.size(), 1u);
  EXPECT_EQ(NegIt->second.terms.at(Arg), -1);

  auto DblIt = states.front().values.find(Dbl);
  ASSERT_NE(DblIt, states.front().values.end());
  EXPECT_FALSE(DblIt->second.top);
  EXPECT_EQ(DblIt->second.constant, 0);
  ASSERT_EQ(DblIt->second.terms.size(), 1u);
  EXPECT_EQ(DblIt->second.terms.at(Arg), -2);
}
