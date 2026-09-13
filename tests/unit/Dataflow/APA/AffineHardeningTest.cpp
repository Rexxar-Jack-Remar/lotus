#include "Dataflow/APA/Domains/AffineTransfer.h"
#include "Dataflow/APA/Analyses/Inter/AffineEqualities.h"
#include "Dataflow/APA/Analyses/Intra/AffineEqualities.h"
#include "TestUtils/LLVMHelpers.h"

#include <chrono>
#include <limits>

#include <gtest/gtest.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace {
using D = elimination::AffineRelationDomain;
using Side = elimination::AffineStateSide;
using Status = elimination::AffineExpressionPrecondition::Status;

elimination::AffineRelationVocabulary vocabulary(llvm::Function &f) {
  elimination::AffineRelationVocabulary result;
  auto add = [&](const llvm::Value *v) {
    if (!elimination::AffineTransferBuilder::isTrackedScalar(v))
      return;
    result.indices[v] = result.values.size();
    result.values.push_back(v);
    result.actualBitWidths[v] = v->getType()->getIntegerBitWidth();
  };
  for (auto &arg : f.args())
    add(&arg);
  for (auto &inst : llvm::instructions(f))
    add(&inst);
  return result;
}

TEST(AffineHardening, SemanticEqualityAndEvenContradictions) {
  llvm::LLVMContext ctx;
  auto m = lotus::unittest::parseModuleChecked(
      ctx, "define void @f(i8 %x) { ret void }");
  auto v = vocabulary(*m->getFunction("f"));
  D::configure(&v);
  auto redundant = D::one();
  redundant.identity = false;
  redundant.components.at(8).constraints.push_back(
      redundant.components.at(8).constraints[0]);
  EXPECT_TRUE(D::equal(redundant, D::one()));
  EXPECT_TRUE(D::contains(redundant, D::one()));
  auto impossible = D::top();
  impossible.components.at(8).constraints = {
      {llvm::APInt(8, 0), llvm::APInt(8, 0), llvm::APInt(8, 2)}};
  EXPECT_TRUE(D::isBottom(impossible));
  EXPECT_TRUE(D::equal(impossible, D::zero()));
}

TEST(AffineHardening, GuardDoesNotDistributeOverHull) {
  llvm::LLVMContext ctx;
  auto m = lotus::unittest::parseModuleChecked(
      ctx, "define void @f(i8 %x) { ret void }");
  auto *x = m->getFunction("f")->getArg(0);
  auto v = vocabulary(*m->getFunction("f"));
  D::configure(&v);
  auto a = D::makeAffineAssignment(x, 0, {});
  auto b = D::makeAffineAssignment(x, 1, {});
  auto guard = D::addPrecondition(D::one(), x, 2);
  EXPECT_FALSE(D::isBottom(D::extend(guard, D::combine(a, b))));
  EXPECT_TRUE(
      D::isBottom(D::combine(D::extend(guard, a), D::extend(guard, b))));
}

TEST(AffineHardening, PhiAndRecursiveArgumentsReadOldState) {
  llvm::LLVMContext ctx;
  auto m = lotus::unittest::parseModuleChecked(ctx, R"(
    define void @f(i8 %a, i8 %b) {
    entry: br label %loop
    loop:
      %x = phi i8 [%a, %entry], [%y, %loop]
      %y = phi i8 [%b, %entry], [%x, %loop]
      call void @f(i8 %b, i8 %a)
      br label %loop
    })");
  auto *f = m->getFunction("f");
  auto v = vocabulary(*f);
  D::configure(&v);
  auto *loop = &f->back();
  auto *x = &loop->front();
  auto *y = x->getNextNode();
  elimination::AffineTransferBuilder builder;
  auto phi = builder.phiTransferForEdge(*loop->getTerminator(), *x);
  auto check = [&](const auto &relation, const llvm::Value *dest,
                   const llvm::Value *src) {
    EXPECT_TRUE(D::entails(
        relation, llvm::APInt(8, 0),
        {{dest, llvm::APInt(8, 1)}, {src, llvm::APInt(8, 255), Side::Pre}}));
  };
  check(phi, x, y);
  check(phi, y, x);
  auto *call = llvm::cast<llvm::CallBase>(y->getNextNode());
  auto entry = builder.callEntryTransfer(*call, *f);
  check(entry, f->getArg(0), f->getArg(1));
  check(entry, f->getArg(1), f->getArg(0));
}

TEST(AffineHardening, SymbolicPreconditionsAndUnknownOutputs) {
  llvm::LLVMContext ctx;
  auto m = lotus::unittest::parseModuleChecked(
      ctx, "define void @f(i8 %x, i8 %y) { ret void }");
  auto *f = m->getFunction("f");
  auto v = vocabulary(*f);
  D::configure(&v);
  auto *x = f->getArg(0);
  auto *y = f->getArg(1);
  auto assignment = D::makeAffineAssignment(y, 3, {{x, 2}});
  auto result = D::expressionPrecondition(assignment, llvm::APInt(8, 1),
                                          {{y, llvm::APInt(8, 1)}});
  ASSERT_EQ(result.status, Status::Exact);
  EXPECT_EQ(result.constant, llvm::APInt(8, 4));
  ASSERT_EQ(result.terms.size(), 1u);
  EXPECT_EQ(result.terms.front().first, x);
  EXPECT_EQ(result.terms.front().second, llvm::APInt(8, 2));
  EXPECT_TRUE(D::equal(result.condition, D::top()));
  auto guarded = D::extend(assignment, D::addPrecondition(D::one(), x, 5));
  auto guarded_result = D::expressionPrecondition(guarded, llvm::APInt(8, 0),
                                                  {{y, llvm::APInt(8, 1)}});
  ASSERT_EQ(guarded_result.status, Status::Exact);
  EXPECT_TRUE(D::entails(guarded_result.condition, llvm::APInt(8, 5),
                         {{x, llvm::APInt(8, 1), Side::Pre}}));
  EXPECT_EQ(D::expressionPrecondition(D::makeForget(y), llvm::APInt(8, 0),
                                      {{y, llvm::APInt(8, 1)}})
                .status,
            Status::Unknown);
  EXPECT_EQ(D::expressionPrecondition(D::zero(), llvm::APInt(8, 0), {}).status,
            Status::Unreachable);
}

TEST(AffineHardening, ResultVocabularySurvivesOtherAnalyses) {
  llvm::LLVMContext ctx;
  auto m = lotus::unittest::parseModuleChecked(ctx, R"(
    define i8 @f() { %x = add i8 20, 22 ret i8 %x }
    define i32 @g(i32 %a) { ret i32 %a }
  )");
  auto *f = m->getFunction("f");
  auto result = elimination::runIntraElimAffineEqualities(f);
  auto other = elimination::runIntraElimAffineEqualities(m->getFunction("g"));
  const auto *fact = result.tryIN(f->back().getTerminator());
  ASSERT_NE(fact, nullptr);
  auto constant = result.getConstant(*fact, &f->front().front());
  ASSERT_TRUE(constant);
  EXPECT_EQ(*constant, llvm::APInt(8, 42));
  EXPECT_EQ(D::componentBitWidth(), 32u);
  auto inter = elimination::runInterElimAffineEqualities(*m);
  D::configure(&other.vocabulary);
  auto scope = inter.scopedVocabulary();
  EXPECT_EQ(D::getVocabulary()->values, inter.vocabulary.values);
}

TEST(AffineHardening, IntraSharedTransfersAndLimitReporting) {
  llvm::LLVMContext ctx;
  auto m = lotus::unittest::parseModuleChecked(ctx, R"(
    define i16 @f(i8 %x) {
      %a = zext i8 42 to i16
      %s = select i1 true, i16 %a, i16 0
      ret i16 %s
    }
    define i8 @loop(i1 %c) {
    entry: br label %body
    body:
      %x = phi i8 [0, %entry], [%next, %body]
      %next = add i8 %x, 1
      br i1 %c, label %body, label %exit
    exit: ret i8 %next
    }
  )");
  auto *f = m->getFunction("f");
  auto result = elimination::runIntraElimAffineEqualities(f);
  auto *ret = llvm::cast<llvm::ReturnInst>(f->back().getTerminator());
  auto value = result.getConstant(*result.tryIN(ret), ret->getReturnValue());
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, llvm::APInt(16, 42));
  for (bool memo : {false, true}) {
    for (auto policy : {elimination::OnNonConvergentStar::Fail,
                        elimination::OnNonConvergentStar::ReturnLast,
                        elimination::OnNonConvergentStar::ReturnIdentity}) {
      elimination::EliminationOptions options;
      options.InterpMemo = memo;
      options.MaxStarIterations = 1;
      options.NonConvergentStarPolicy = policy;
      auto limited = elimination::runIntraElimAffineEqualities(
          m->getFunction("loop"), options);
      EXPECT_EQ(limited.solveStatus(),
                elimination::SolveStatus::NonConvergentStar);
      EXPECT_TRUE(limited.solveDiagnostics().max_star_hit);
      EXPECT_GT(limited.solveDiagnostics().star_iterations_total, 0u);
    }
  }
}

TEST(AffineHardening, CachedCompositionMatchesUncachedAndAvoidsWork) {
  llvm::LLVMContext ctx;
  auto m = lotus::unittest::parseModuleChecked(
      ctx, "define void @f(i64 %x) { ret void }");
  auto *x = m->getFunction("f")->getArg(0);
  auto v = vocabulary(*m->getFunction("f"));
  D::configure(&v);
  auto a = D::makeAffineAssignment(x, std::numeric_limits<int64_t>::min(),
                                   {{x, 1}, {x, 1}});
  auto b = D::makeAffineAssignment(x, 1, {{x, 1}});
  D::setCacheCapacity(0);
  auto expected = D::extend(a, b);
  D::setCacheCapacity(128);
  for (unsigned i = 0; i < 20; ++i)
    EXPECT_TRUE(D::equal(D::extend(a, b), expected));
  auto stats = D::cacheStatistics();
  EXPECT_EQ(stats.compositions, 20u);
  EXPECT_EQ(stats.compositionHits, 19u);
  EXPECT_GT(stats.normalizationHits, 0u);
  D::configure(&v);
  EXPECT_EQ(D::cacheStatistics().compositionHits, 0u);
}

TEST(AffineHardening, PullbackMatchesEnumeratedTransitions) {
  llvm::LLVMContext ctx;
  auto m = lotus::unittest::parseModuleChecked(
      ctx, "define void @f(i3 %x) { ret void }");
  auto *x = m->getFunction("f")->getArg(0);
  auto v = vocabulary(*m->getFunction("f"));
  D::configure(&v);
  auto satisfies = [](const auto &relation, unsigned pre, unsigned post) {
    if (D::isBottom(relation))
      return false;
    for (const auto &row : relation.components.at(3).constraints)
      if (!(row[0] * llvm::APInt(3, pre) + row[1] * llvm::APInt(3, post) +
            row[2])
               .isZero())
        return false;
    return true;
  };
  for (unsigned a = 0; a < 8; ++a) {
    for (unsigned b = 0; b < 8; ++b) {
      auto assignment = D::makeAffineAssignment(x, b, {{x, a}});
      for (const auto &relation :
           {assignment, D::combine(assignment, D::one()),
            D::extend(assignment, D::addPrecondition(D::one(), x, b))}) {
        auto pulled = D::expressionPrecondition(relation, llvm::APInt(3, 1),
                                                {{x, llvm::APInt(3, 1)}});
        for (unsigned pre = 0; pre < 8; ++pre) {
          bool feasible = false;
          for (unsigned post = 0; post < 8; ++post) {
            if (!satisfies(relation, pre, post))
              continue;
            feasible = true;
            ASSERT_NE(pulled.status, Status::Unreachable);
            if (pulled.status == Status::Exact) {
              auto value = pulled.constant;
              for (const auto &term : pulled.terms)
                value += term.second * llvm::APInt(3, pre);
              EXPECT_EQ(value, llvm::APInt(3, post + 1));
            }
          }
          EXPECT_EQ(satisfies(pulled.condition, pre, 0), feasible);
        }
      }
    }
  }
}

TEST(AffineHardening, GuardedClientRestrictsRequestedLaws) {
  llvm::LLVMContext ctx;
  auto m = lotus::unittest::parseModuleChecked(ctx, R"(
    declare void @llvm.assume(i1)
    define i8 @f(i1 %c) {
      br i1 %c, label %left, label %right
    left: br label %join
    right: br label %join
    join:
      %x = phi i8 [0, %left], [1, %right]
      %p = icmp eq i8 %x, 2
      call void @llvm.assume(i1 %p)
      ret i8 %x
    })");
  auto *f = m->getFunction("f");
  auto baseline = elimination::runIntraElimAffineEqualities(f);
  for (bool memo : {false, true}) {
    elimination::EliminationOptions options;
    options.InterpMemo = memo;
    options.EnableEAN = true;
    options.EANLaws = elimination::ean::LawProfile::kleeneAlgebra();
    auto result = elimination::runIntraElimAffineEqualities(f, options);
    EXPECT_TRUE(result.solveDiagnostics().ean_laws_restricted);
    auto scope = result.scopedVocabulary();
    for (auto *inst : result.nodes()) {
      ASSERT_NE(baseline.tryIN(inst), nullptr);
      EXPECT_TRUE(D::equal(*baseline.tryIN(inst), *result.tryIN(inst)));
    }
  }
}
} // namespace
