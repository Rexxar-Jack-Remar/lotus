// Intraprocedural affine-equalities EAN client tests.
//
// Assignments distribute over affine hull, but guards need not. The client
// restricts requested profiles to prefix factoring and memoizes by input fact.

#include "Dataflow/APA/Analyses/Intra/AffineEqualities.h"
#include "Dataflow/APA/Domains/AffineRelationDomain.h"
#include "TestUtils/LLVMHelpers.h"

#include <map>
#include <vector>

#include <gtest/gtest.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace {

using elimination::AffineEqualitiesResult;
using elimination::AffineFact;
using D = elimination::AffineRelationDomain;

// Build + install a vocabulary of tracked integer scalars of the single
// dominant bit-width (mirrors the analysis's own rule). Kept alive by the
// caller so the static domain configuration stays valid while we inspect facts.
elimination::AffineRelationVocabulary buildVocab(llvm::Function &F) {
  auto widthOf = [](const llvm::Value *V) -> unsigned {
    auto *IT = llvm::dyn_cast<llvm::IntegerType>(V->getType());
    return (IT && IT->getBitWidth() <= 64) ? IT->getBitWidth() : 0;
  };
  std::map<unsigned, unsigned> Counts;
  for (auto &Arg : F.args())
    if (unsigned W = widthOf(&Arg))
      ++Counts[W];
  for (auto &I : llvm::instructions(F))
    if (unsigned W = widthOf(&I))
      ++Counts[W];
  unsigned Dom = 0, Best = 0;
  for (auto &[W, C] : Counts)
    if (C > Best || (C == Best && W > Dom)) {
      Dom = W;
      Best = C;
    }

  elimination::AffineRelationVocabulary Vocab;
  auto Add = [&](const llvm::Value *V) {
    if (widthOf(V) != Dom || Dom == 0 || Vocab.indices.count(V))
      return;
    const unsigned Idx = static_cast<unsigned>(Vocab.values.size());
    Vocab.indices.emplace(V, Idx);
    Vocab.actualBitWidths.emplace(V, Dom);
    Vocab.values.push_back(V);
  };
  for (auto &Arg : F.args())
    Add(&Arg);
  for (auto &I : llvm::instructions(F))
    Add(&I);
  return Vocab;
}

elimination::EliminationOptions defaultOpts() { return {}; }
elimination::EliminationOptions eanSafeOpts() {
  elimination::EliminationOptions O;
  O.EnableEAN = true;
  O.EANLaws = elimination::ean::LawProfile::safeMinimal();
  return O;
}
elimination::EliminationOptions eanKleeneOpts() {
  elimination::EliminationOptions O;
  O.EnableEAN = true;
  O.EANLaws = elimination::ean::LawProfile::kleeneAlgebra();
  return O;
}
elimination::EliminationOptions greedyOpts() {
  elimination::EliminationOptions O;
  O.EnableGreedy = true;
  return O;
}
// A function with affine assignments, a merge, and a loop (Star).
const char *kLoopFn = R"LL(
define i32 @f(i32 %n) {
entry:
  %a = add i32 %n, 1
  br label %loop
loop:
  %i = phi i32 [ %a, %entry ], [ %i2, %loop ]
  %i2 = add i32 %i, 2
  %c = icmp slt i32 %i2, 100
  br i1 %c, label %loop, label %exit
exit:
  %r = add i32 %i2, 5
  ret i32 %r
}
)LL";

// Assert that a config reproduces the Default facts at every instruction.
void expectSameFacts(llvm::Function &F,
                     const elimination::EliminationOptions &O,
                     const char *Label) {
  const AffineEqualitiesResult Base =
      elimination::runIntraElimAffineEqualities(&F, defaultOpts());
  const AffineEqualitiesResult Got =
      elimination::runIntraElimAffineEqualities(&F, O);
  auto scope = Got.scopedVocabulary();
  unsigned Idx = 0;
  for (auto &I : llvm::instructions(F)) {
    const auto *B = Base.tryIN(&I);
    const auto *G = Got.tryIN(&I);
    ASSERT_EQ(B != nullptr, G != nullptr) << Label << " presence @inst#" << Idx;
    if (B != nullptr && G != nullptr) {
      EXPECT_TRUE(D::equal(*B, *G)) << Label << " fact differs @inst#" << Idx;
    }
    ++Idx;
  }
}

TEST(AffineEan, TransferDistributesOverJoin) {
  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseModuleChecked(
      Ctx, "define void @g(i32 %x, i32 %y) {\n  ret void\n}\n");
  auto *G = M->getFunction("g");
  ASSERT_NE(G, nullptr);
  auto Vocab = buildVocab(*G);
  D::configure(&Vocab);

  auto *X = G->getArg(0);
  auto *Y = G->getArg(1);
  // a: x' = 5 ; b: identity ; T: y' = x
  const AffineFact A = D::makeAffineAssignment(X, 5, {});
  const AffineFact B = D::identity();
  const AffineFact T = D::makeAffineAssignment(Y, 0, {{X, 1}});

  // Sanity: the domain builders must produce informative (non-bottom,
  // non-identity) relations, else the equalities below hold vacuously.
  ASSERT_FALSE(D::isBottom(A)) << "makeAffineAssignment produced bottom";
  ASSERT_FALSE(D::equal(A, D::identity())) << "x'=5 collapsed to identity";
  ASSERT_FALSE(D::isBottom(D::extend(T, B)))
      << "extend(assignment, identity) produced bottom";

  const AffineFact Lhs = D::extend(T, D::combine(A, B));
  const AffineFact Rhs = D::combine(D::extend(T, A), D::extend(T, B));
  EXPECT_TRUE(D::equal(Lhs, Rhs))
      << "affine transfer must distribute over the join (kleene soundness)";
}

TEST(AffineEan, DomainComposeThreeVars) {
  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseModuleChecked(
      Ctx, "define void @g(i32 %x, i32 %y, i32 %z) {\n  ret void\n}\n");
  auto *G = M->getFunction("g");
  ASSERT_NE(G, nullptr);
  auto Vocab = buildVocab(*G); // {x,y,z}, all i32
  D::configure(&Vocab);
  auto *X = G->getArg(0);
  auto *Y = G->getArg(1);
  std::string Bad;
  for (int64_t C : {0, 1, 2, 3, 4, 5, 6, 8, 16}) {
    const AffineFact T = D::makeAffineAssignment(Y, C, {{X, 1}}); // y' = x + C
    if (D::isBottom(D::extend(T, D::identity())))
      Bad += " " + std::to_string(C);
  }
  EXPECT_TRUE(Bad.empty()) << "extend(y'=x+C, identity) bottom for C in:"
                           << Bad;
}

TEST(AffineEan, ComputesNonTrivialAffineFacts) {
  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseModuleChecked(Ctx, "define i32 @s(i32 %n) {\n"
                                                    "  %a = add i32 %n, 1\n"
                                                    "  %b = add i32 %a, 2\n"
                                                    "  ret i32 %b\n"
                                                    "}\n");
  auto *F = M->getFunction("s");
  ASSERT_NE(F, nullptr);

  const AffineEqualitiesResult Res =
      elimination::runIntraElimAffineEqualities(F, {});
  auto Vocab = buildVocab(*F);
  D::configure(&Vocab);
  unsigned Total = 0, NonNull = 0, Bottom = 0, Informative = 0;
  for (auto &I : llvm::instructions(*F)) {
    ++Total;
    const auto *Fact = Res.tryIN(&I);
    if (Fact == nullptr)
      continue;
    ++NonNull;
    if (D::isBottom(*Fact)) {
      ++Bottom;
    } else if (!D::equal(*Fact, D::identity())) {
      ++Informative;
    }
  }
  // Facts must be populated (preservation tests would be vacuous otherwise),
  // reachable code must not be bottom, and the analysis must capture at least
  // one non-identity affine relation.
  EXPECT_EQ(NonNull, Total) << "some IN facts missing";
  EXPECT_EQ(Bottom, 0u) << "reachable code should not be bottom";
  EXPECT_GT(Informative, 0u)
      << "no informative affine fact (Total=" << Total << " NonNull=" << NonNull
      << " Bottom=" << Bottom << ")";
}

TEST(AffineEan, EanSafePreservesFacts) {
  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseModuleChecked(Ctx, kLoopFn);
  auto *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);
  expectSameFacts(*F, eanSafeOpts(), "EAN(safe)");
}

// A full Kleene request is restricted to the client's supported laws.
TEST(AffineEan, EanKleenePreservesFacts) {
  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseModuleChecked(Ctx, kLoopFn);
  auto *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);
  expectSameFacts(*F, eanKleeneOpts(), "EAN(kleene)");
}

TEST(AffineEan, GreedyPreservesFacts) {
  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseModuleChecked(Ctx, kLoopFn);
  auto *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);
  expectSameFacts(*F, greedyOpts(), "Greedy");
}

// The memoizing transformer interpreter must produce IDENTICAL facts to the
// tree-walking interpreter (semantic equivalence), both with and without EAN.
TEST(AffineEan, MemoInterpreterMatchesTreeInterpreter) {
  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseModuleChecked(Ctx, kLoopFn);
  auto *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  auto memo = [](elimination::EliminationOptions O) {
    O.InterpMemo = true;
    return O;
  };
  // Tree (baseline) vs memo, under Default and under EAN(kleene).
  for (const auto &Base : {defaultOpts(), eanKleeneOpts()}) {
    const AffineEqualitiesResult Tree =
        elimination::runIntraElimAffineEqualities(F, Base);
    const AffineEqualitiesResult Memo =
        elimination::runIntraElimAffineEqualities(F, memo(Base));
    auto scope = Memo.scopedVocabulary();
    unsigned Idx = 0;
    for (auto &I : llvm::instructions(*F)) {
      const auto *T = Tree.tryIN(&I);
      const auto *Mm = Memo.tryIN(&I);
      ASSERT_EQ(T != nullptr, Mm != nullptr) << "presence @inst#" << Idx;
      if (T != nullptr && Mm != nullptr) {
        EXPECT_TRUE(D::equal(*T, *Mm)) << "memo != tree @inst#" << Idx;
      }
      ++Idx;
    }
  }
}

} // namespace
