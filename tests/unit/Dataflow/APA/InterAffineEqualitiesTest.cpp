#include "Dataflow/APA/Analyses/Inter/InterAffineEqualities.h"

#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <iterator>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

namespace {

using lotus::unittest::findInstructionByName;
using lotus::unittest::parseModule;

std::vector<elimination::AffineState> materializedAffineStatesForBlock(
    const std::map<elimination::BlockKey, elimination::AffineRelationDomain::value_type> &facts,
    const llvm::BasicBlock *block) {
  std::vector<elimination::AffineState> out;
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    out.push_back(elimination::materializeAffineExpressions(entry.second));
  }
  return out;
}

std::vector<const elimination::AffineRelationDomain::value_type *> relationsForBlock(
    const std::map<elimination::BlockKey, elimination::AffineRelationDomain::value_type> &facts,
    const llvm::BasicBlock *block) {
  std::vector<const elimination::AffineRelationDomain::value_type *> out;
  for (const auto &entry : facts) {
    if (entry.first.block != block)
      continue;
    out.push_back(&entry.second);
  }
  return out;
}

bool equalityMatchesUpToNegation(
    const elimination::AffineEquality &equality, int64_t constant,
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

bool stateHasEquality(
    const elimination::AffineState &state, int64_t constant,
    std::initializer_list<std::pair<const llvm::Value *, int64_t>> terms) {
  return std::any_of(state.equalities.begin(), state.equalities.end(),
                     [&](const elimination::AffineEquality &equality) {
                       return equalityMatchesUpToNegation(equality, constant,
                                                          terms);
                     });
}

int64_t congruenceScale(unsigned modulusBits) {
  unsigned width = elimination::AffineRelationDomain::componentBitWidth();
  return static_cast<int64_t>(uint64_t{1} << (width - modulusBits));
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

TEST(InterAffineEqualities, CompareEquivalentAffineExpressionsProducesConstant) {
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

  auto result = elimination::InterAffineEqualities::run(*module);
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

  auto result = elimination::InterAffineEqualities::run(*module);
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

  auto result = elimination::InterAffineEqualities::run(*module);
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

TEST(InterAffineEqualities, TruncKeepsLowBitCongruence) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %narrow = trunc i32 %x to i8
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = &*Main->arg_begin();
  auto *Narrow = findInstructionByName(*Main, "narrow");
  ASSERT_NE(Narrow, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  const int64_t Mod256Scale = congruenceScale(8);
  EXPECT_TRUE(stateHasEquality(states.front(), 0,
                               {{Narrow, Mod256Scale}, {X, -Mod256Scale}}));
}

TEST(InterAffineEqualities, ZextKeepsSourceLowBitCongruence) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i8 %x) {
    entry:
      %wide = zext i8 %x to i32
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = &*Main->arg_begin();
  auto *Wide = findInstructionByName(*Main, "wide");
  ASSERT_NE(Wide, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  const int64_t Mod256Scale = congruenceScale(8);
  EXPECT_TRUE(stateHasEquality(states.front(), 0,
                               {{Wide, Mod256Scale}, {X, -Mod256Scale}}));
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

TEST(InterAffineEqualities, FreezePreservesAffineValue) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %sum = add i32 %x, 5
      %frozen = freeze i32 %sum
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *X = &*Main->arg_begin();
  auto *Frozen = findInstructionByName(*Main, "frozen");
  ASSERT_NE(Frozen, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front().values.find(Frozen);
  ASSERT_NE(It, states.front().values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 5);
  ASSERT_EQ(It->second.terms.size(), 1u);
  EXPECT_EQ(It->second.terms.at(X), 1);
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

  auto result = elimination::InterAffineEqualities::run(*module);
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

TEST(InterAffineEqualities, LogicalRightShiftKeepsExactAffineQuotientWhenDivisible) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %scaled = shl i32 %x, 4
      %shr = lshr i32 %scaled, 2
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Arg = &*Main->arg_begin();
  auto *Shr = findInstructionByName(*Main, "shr");
  ASSERT_NE(Shr, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front().values.find(Shr);
  ASSERT_NE(It, states.front().values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 0);
  ASSERT_EQ(It->second.terms.size(), 1u);
  EXPECT_EQ(It->second.terms.at(Arg), 4);
}

TEST(InterAffineEqualities, UnsignedDivideByPowerOfTwoKeepsExactAffineQuotientWhenDivisible) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @main(i32 %x) {
    entry:
      %scaled = shl i32 %x, 5
      %q = udiv i32 %scaled, 8
      br label %next

    next:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto *Main = module->getFunction("main");
  ASSERT_NE(Main, nullptr);
  auto *Arg = &*Main->arg_begin();
  auto *Q = findInstructionByName(*Main, "q");
  ASSERT_NE(Q, nullptr);
  auto NextIt = std::next(Main->begin());
  ASSERT_NE(NextIt, Main->end());
  auto *Next = &*NextIt;

  auto result = elimination::InterAffineEqualities::run(*module);
  auto states = materializedAffineStatesForBlock(result.blockRelations, Next);
  ASSERT_EQ(states.size(), 1u);

  auto It = states.front().values.find(Q);
  ASSERT_NE(It, states.front().values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 0);
  ASSERT_EQ(It->second.terms.size(), 1u);
  EXPECT_EQ(It->second.terms.at(Arg), 4);
}

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
  EXPECT_TRUE(stateHasEquality(states.front(), 0,
                               {{Y, Mod16Scale}, {X, -Mod16Scale}}));
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
  EXPECT_TRUE(stateHasEquality(states.front(), 0,
                               {{Y, Mod8Scale}, {X, -Mod8Scale}}));
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
  EXPECT_TRUE(stateHasEquality(states.front(), 0,
                               {{Y, Mod4Scale}, {X, -Mod4Scale}}));
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
  EXPECT_TRUE(stateHasEquality(states.front(), 0,
                               {{Y, Mod4Scale}, {X, -Mod4Scale}}));
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
  EXPECT_TRUE(stateHasEquality(states.front(), 0,
                               {{Z, Mod16Scale}, {Y, -Mod16Scale}}));
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
  EXPECT_TRUE(stateHasEquality(states.front(), 0,
                               {{Z, Mod16Scale}, {Y, -Mod16Scale}}));
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
