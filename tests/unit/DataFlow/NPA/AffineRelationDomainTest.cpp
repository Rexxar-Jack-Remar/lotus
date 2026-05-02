#include "Dataflow/NPA/Domains/AffineRelationDomain.h"

#include "Dataflow/NPA/Analyses/Inter/InterAffineEqualities.h"
#include "Dataflow/NPA/Core/Base/Foundation.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace {

using lotus::unittest::parseModule;

npa::AffineRelationVocabulary buildVocabulary(const llvm::Module &M) {
  npa::AffineRelationVocabulary vocab;
  std::unordered_set<const llvm::Value *> seen;
  auto record = [&](const llvm::Value *value) {
    if (seen.insert(value).second)
      vocab.values.push_back(value);
  };
  for (const auto &F : M) {
    if (F.isDeclaration())
      continue;
    for (const auto &Arg : F.args()) {
      if (Arg.getType()->isIntegerTy() &&
          Arg.getType()->getIntegerBitWidth() <= 64)
        record(&Arg);
    }
    for (const auto &BB : F) {
      for (const auto &I : BB) {
        if (I.getType()->isIntegerTy() &&
            I.getType()->getIntegerBitWidth() <= 64)
          record(&I);
      }
    }
  }
  for (unsigned i = 0; i < vocab.values.size(); ++i) {
    vocab.indices[vocab.values[i]] = i;
    vocab.actualBitWidths[vocab.values[i]] =
        vocab.values[i]->getType()->getIntegerBitWidth();
  }
  return vocab;
}

using Matrix = std::vector<std::vector<llvm::APInt>>;

Matrix makeMatrix(unsigned bitWidth,
                  std::initializer_list<std::initializer_list<uint64_t>> rows) {
  Matrix out;
  for (auto row : rows) {
    out.emplace_back();
    for (uint64_t value : row)
      out.back().emplace_back(bitWidth, value);
  }
  return out;
}

bool hasMatrix(const std::vector<Matrix> &matrices, const Matrix &expected) {
  return std::find(matrices.begin(), matrices.end(), expected) !=
         matrices.end();
}

llvm::APInt dot(const std::vector<llvm::APInt> &lhs,
                const std::vector<llvm::APInt> &rhs) {
  llvm::APInt out(lhs.front().getBitWidth(), 0);
  for (size_t i = 0; i < lhs.size(); ++i)
    out += lhs[i] * rhs[i];
  return out;
}

bool rowsAreOrthogonal(const Matrix &lhs, const Matrix &rhs) {
  for (const auto &lhsRow : lhs) {
    for (const auto &rhsRow : rhs) {
      if (!dot(lhsRow, rhsRow).isZero())
        return false;
    }
  }
  return true;
}

Matrix multiply(const Matrix &lhs, const Matrix &rhs, unsigned bitWidth) {
  if (lhs.empty() || rhs.empty())
    return {};
  Matrix out(lhs.size(), std::vector<llvm::APInt>(rhs.front().size(),
                                                  llvm::APInt(bitWidth, 0)));
  for (size_t r = 0; r < lhs.size(); ++r) {
    for (size_t k = 0; k < rhs.size(); ++k) {
      for (size_t c = 0; c < rhs.front().size(); ++c)
        out[r][c] += lhs[r][k] * rhs[k][c];
    }
  }
  return out;
}

} // namespace

TEST(AffineRelationDomain, IdentityEqualsItself) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto id1 = npa::AffineRelationDomain::identity();
  auto id2 = npa::AffineRelationDomain::identity();
  EXPECT_TRUE(npa::AffineRelationDomain::equal(id1, id2));
}

TEST(AffineRelationDomain, ComposeWithIdentityPreservesAssignment) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      %y = add i32 %x, 4
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *X = &*F->arg_begin();
  auto *Y = &*F->getEntryBlock().begin();

  auto assign = npa::AffineRelationDomain::makeAffineAssignment(Y, 4, {{X, 1}});
  auto composed = npa::AffineRelationDomain::extend(
      assign, npa::AffineRelationDomain::identity());
  auto state = npa::materializeAffineExpressions(composed);

  auto It = state.values.find(Y);
  ASSERT_NE(It, state.values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 4);
  ASSERT_EQ(It->second.terms.size(), 1u);
  EXPECT_EQ(It->second.terms.at(X), 1);
}

TEST(AffineRelationDomain, GuardMaterializesConstant) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %tag) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto *Tag = &*module->getFunction("f")->arg_begin();
  auto guarded = npa::AffineRelationDomain::addPrecondition(
      npa::AffineRelationDomain::identity(), Tag, 7);
  auto state = npa::materializeAffineExpressions(guarded);

  auto It = state.values.find(Tag);
  ASSERT_NE(It, state.values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 7);
  EXPECT_TRUE(It->second.terms.empty());
}

TEST(AffineRelationDomain, BottomIsDistinctFromIdentity) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto bottom = npa::AffineRelationDomain::zero();
  auto id = npa::AffineRelationDomain::identity();
  EXPECT_FALSE(npa::AffineRelationDomain::equal(bottom, id));
  EXPECT_TRUE(npa::AffineRelationDomain::isBottom(bottom));
  EXPECT_FALSE(npa::AffineRelationDomain::isBottom(id));
}

TEST(AffineRelationDomain, TopContainsIdentityAndBottom) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto top = npa::AffineRelationDomain::top();
  auto id = npa::AffineRelationDomain::identity();
  auto bottom = npa::AffineRelationDomain::zero();

  EXPECT_TRUE(npa::AffineRelationDomain::contains(top, id));
  EXPECT_TRUE(npa::AffineRelationDomain::contains(top, bottom));
  EXPECT_TRUE(npa::AffineRelationDomain::contains(id, bottom));
  EXPECT_FALSE(npa::AffineRelationDomain::contains(id, top));
}

TEST(AffineRelationDomain, AffineGeneratorRoundTripPreservesKSRelation) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      %y = add i32 %x, 4
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *X = &*F->arg_begin();
  auto *Y = &*F->getEntryBlock().begin();

  auto relation =
      npa::AffineRelationDomain::makeAffineAssignment(Y, 4, {{X, 1}});
  auto ag = npa::AffineRelationDomain::toAffineGenerator(relation);
  auto roundTrip = npa::AffineRelationDomain::fromAffineGenerator(ag);

  EXPECT_TRUE(ag.exact);
  ASSERT_FALSE(ag.generators.empty());
  EXPECT_FALSE(ag.generators.begin()->second.empty());
  EXPECT_TRUE(npa::AffineRelationDomain::equal(relation, roundTrip));
}

TEST(AffineRelationDomain, AffineGeneratorJoinUsesKSJoinSemantics) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      %y = add i32 %x, 4
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *X = &*F->arg_begin();
  auto *Y = &*F->getEntryBlock().begin();

  auto assign = npa::AffineRelationDomain::makeAffineAssignment(Y, 4, {{X, 1}});
  auto id = npa::AffineRelationDomain::identity();
  auto joined = npa::AffineRelationDomain::joinAffineGenerators(
      npa::AffineRelationDomain::toAffineGenerator(assign),
      npa::AffineRelationDomain::toAffineGenerator(id));

  EXPECT_TRUE(npa::AffineRelationDomain::equal(
      npa::AffineRelationDomain::fromAffineGenerator(joined),
      npa::AffineRelationDomain::combine(assign, id)));
}

TEST(AffineRelationDomain, PublicDualizePerpComputesOrthogonalComplement) {
  Matrix constraints = makeMatrix(4, {
                                         {1, 15, 0},
                                     });

  auto decomposition =
      npa::AffineRelationDomain::diagonalDecompose(constraints, 4);
  auto dual = npa::AffineRelationDomain::dualizePerp(constraints, 4, 3);
  auto roundTrip = npa::AffineRelationDomain::dualizePerp(dual, 4, 3);

  EXPECT_EQ(decomposition.bitWidth, 4u);
  EXPECT_EQ(decomposition.dual, dual);
  EXPECT_TRUE(rowsAreOrthogonal(constraints, dual));
  EXPECT_TRUE(rowsAreOrthogonal(dual, roundTrip));
  EXPECT_EQ(roundTrip, constraints);
  EXPECT_EQ(multiply(multiply(decomposition.left, decomposition.diagonal, 4),
                     decomposition.right, 4),
            constraints);
  EXPECT_EQ(multiply(decomposition.left, decomposition.leftInverse, 4),
            makeMatrix(4, {{1}}));
  EXPECT_EQ(multiply(decomposition.right, decomposition.rightInverse, 4),
            makeMatrix(4, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}));
}

TEST(AffineRelationDomain, DiagonalDecomposeTracksRectangularFactors) {
  Matrix constraints = makeMatrix(4, {
                                         {2, 6, 4},
                                         {0, 4, 8},
                                     });

  auto decomposition =
      npa::AffineRelationDomain::diagonalDecompose(constraints, 4);
  auto dual = npa::AffineRelationDomain::dualizePerp(constraints, 4, 3);

  EXPECT_EQ(multiply(multiply(decomposition.left, decomposition.diagonal, 4),
                     decomposition.right, 4),
            constraints);
  EXPECT_EQ(multiply(decomposition.left, decomposition.leftInverse, 4),
            makeMatrix(4, {{1, 0}, {0, 1}}));
  EXPECT_EQ(multiply(decomposition.right, decomposition.rightInverse, 4),
            makeMatrix(4, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}));
  EXPECT_EQ(decomposition.dual, dual);
  EXPECT_TRUE(rowsAreOrthogonal(constraints, dual));
}

TEST(AffineRelationDomain, AffineGeneratorKSRoundTripPreservesGeneratorSpace) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i4 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  npa::AffineGeneratorRelation ag;
  ag.generators[4] = makeMatrix(4, {
                                       {1, 0, 0},
                                       {0, 1, 1},
                                   });

  auto relation = npa::AffineRelationDomain::fromAffineGenerator(ag);
  auto roundTripAG = npa::AffineRelationDomain::toAffineGenerator(relation);
  auto roundTripKS =
      npa::AffineRelationDomain::fromAffineGenerator(roundTripAG);

  ASSERT_FALSE(roundTripAG.generators.empty());
  EXPECT_FALSE(roundTripAG.generators.begin()->second.empty());
  EXPECT_TRUE(npa::AffineRelationDomain::equal(relation, roundTripKS));
}

TEST(AffineRelationDomain, MOSRoundTripPreservesKSRelation) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      %y = add i32 %x, 4
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *X = &*F->arg_begin();
  auto *Y = &*F->getEntryBlock().begin();

  auto relation =
      npa::AffineRelationDomain::makeAffineAssignment(Y, 4, {{X, 1}});
  auto mos = npa::AffineRelationDomain::toMOSWithMakeExplicit(relation);
  auto roundTrip = npa::AffineRelationDomain::fromMOS(mos);

  EXPECT_TRUE(mos.exact);
  EXPECT_EQ(mos.kind, npa::MOSRelation::ConversionKind::MakeExplicit);
  ASSERT_FALSE(mos.transformers.empty());
  EXPECT_FALSE(mos.transformers.begin()->second.empty());
  EXPECT_TRUE(npa::AffineRelationDomain::equal(relation, roundTrip));
}

TEST(AffineRelationDomain, MOSToKSConvertsExplicitAffineTransformer) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i4 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto *X = &*module->getFunction("f")->arg_begin();
  npa::MOSRelation mos;
  mos.transformers[4] = {
      makeMatrix(4, {{1, 3}, {0, 5}}),
  };

  auto relation = npa::AffineRelationDomain::fromMOS(mos);
  auto state = npa::materializeAffineExpressions(relation);

  auto It = state.values.find(X);
  ASSERT_NE(It, state.values.end());
  EXPECT_FALSE(It->second.top);
  EXPECT_EQ(It->second.constant, 3);
  ASSERT_EQ(It->second.terms.size(), 1u);
  EXPECT_EQ(It->second.terms.at(X), 5);
}

TEST(AffineRelationDomain, GuardedKSToMOSVariantsProduceDistinctTransformers) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i4 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  npa::AffineGeneratorRelation ag;
  ag.generators[4] = makeMatrix(4, {
                                       {1, 0, 0},
                                       {0, 4, 12},
                                   });
  ag.relation = npa::AffineRelationDomain::fromAffineGenerator(ag);

  auto havoc =
      npa::AffineRelationDomain::toMOSWithHavocedPreStateGuards(ag.relation);
  auto explicitMOS =
      npa::AffineRelationDomain::toMOSWithMakeExplicit(ag.relation);

  ASSERT_EQ(havoc.transformers.count(4), 1u);
  ASSERT_EQ(explicitMOS.transformers.count(4), 1u);

  EXPECT_TRUE(
      hasMatrix(havoc.transformers.at(4), makeMatrix(4, {{1, 0}, {0, 0}})));
  EXPECT_TRUE(
      hasMatrix(havoc.transformers.at(4), makeMatrix(4, {{0, 4}, {0, 0}})));
  EXPECT_TRUE(hasMatrix(explicitMOS.transformers.at(4),
                        makeMatrix(4, {{1, 0}, {0, 3}})));
  EXPECT_FALSE(
      hasMatrix(havoc.transformers.at(4), makeMatrix(4, {{1, 0}, {0, 3}})));
}

TEST(AffineRelationDomain, MakeExplicitHandlesSkippedPreStateColumns) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i4 %x, i4 %y) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  npa::AffineGeneratorRelation ag;
  ag.generators[4] = makeMatrix(4, {
                                       {1, 0, 0, 0, 0},
                                       {0, 0, 1, 0, 7},
                                   });
  ag.relation = npa::AffineRelationDomain::fromAffineGenerator(ag);

  auto explicitMOS =
      npa::AffineRelationDomain::toMOSWithMakeExplicit(ag.relation);

  ASSERT_EQ(explicitMOS.transformers.count(4), 1u);
  ASSERT_FALSE(explicitMOS.transformers.at(4).empty());
  EXPECT_FALSE(npa::AffineRelationDomain::isBottom(
      npa::AffineRelationDomain::fromMOS(explicitMOS)));
}

TEST(AffineRelationDomain, MakeExplicitSplitsEvenPreStateLeadingValues) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i4 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  npa::AffineGeneratorRelation ag;
  ag.generators[4] = makeMatrix(4, {
                                       {1, 0, 0},
                                       {0, 2, 6},
                                   });
  ag.relation = npa::AffineRelationDomain::fromAffineGenerator(ag);

  auto explicitMOS =
      npa::AffineRelationDomain::toMOSWithMakeExplicit(ag.relation);

  ASSERT_EQ(explicitMOS.transformers.count(4), 1u);
  EXPECT_TRUE(hasMatrix(explicitMOS.transformers.at(4),
                        makeMatrix(4, {{1, 0}, {0, 3}})));
}

TEST(AffineRelationDomain, MOSJoinUsesKSJoinSemantics) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      %y = add i32 %x, 4
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *X = &*F->arg_begin();
  auto *Y = &*F->getEntryBlock().begin();

  auto assign = npa::AffineRelationDomain::makeAffineAssignment(Y, 4, {{X, 1}});
  auto id = npa::AffineRelationDomain::identity();
  auto joined = npa::AffineRelationDomain::joinMOS(
      npa::AffineRelationDomain::toMOS(assign),
      npa::AffineRelationDomain::toMOS(id));

  EXPECT_EQ(joined.kind, npa::MOSRelation::ConversionKind::Direct);
  EXPECT_TRUE(npa::AffineRelationDomain::equal(
      npa::AffineRelationDomain::fromMOS(joined),
      npa::AffineRelationDomain::combine(assign, id)));
}

TEST(AffineRelationDomain, CondCombineRespectsBooleanGuard) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto id = npa::AffineRelationDomain::identity();
  auto bottom = npa::AffineRelationDomain::zero();

  EXPECT_TRUE(npa::AffineRelationDomain::equal(
      npa::AffineRelationDomain::condCombine(true, id, bottom), id));
  EXPECT_TRUE(npa::AffineRelationDomain::equal(
      npa::AffineRelationDomain::condCombine(false, id, bottom), bottom));
}

TEST(AffineRelationDomain, MeetAddsConstraintsExactly) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x, i32 %y) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *ArgIt = F->arg_begin();
  auto *X = &*ArgIt;
  ++ArgIt;
  auto *Y = &*ArgIt;

  auto xIsThree = npa::AffineRelationDomain::addPrecondition(
      npa::AffineRelationDomain::identity(), X, 3);
  auto yIsFive = npa::AffineRelationDomain::addPrecondition(
      npa::AffineRelationDomain::identity(), Y, 5);
  auto both = npa::AffineRelationDomain::meet(xIsThree, yIsFive);
  auto state = npa::materializeAffineExpressions(both);

  ASSERT_NE(state.values.find(X), state.values.end());
  ASSERT_NE(state.values.find(Y), state.values.end());
  EXPECT_EQ(state.values.at(X).constant, 3);
  EXPECT_EQ(state.values.at(Y).constant, 5);
}

TEST(AffineRelationDomain, ProjectAndHavocRemoveSelectedVocabulary) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %x, i32 %y) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *ArgIt = F->arg_begin();
  auto *X = &*ArgIt;
  ++ArgIt;
  auto *Y = &*ArgIt;

  auto relation = npa::AffineRelationDomain::addPrecondition(
      npa::AffineRelationDomain::identity(), X, 7);
  relation = npa::AffineRelationDomain::addPrecondition(relation, Y, 11);

  auto onlyX = npa::AffineRelationDomain::projectOnto(relation, {X});
  auto onlyXState = npa::materializeAffineExpressions(onlyX);
  ASSERT_NE(onlyXState.values.find(X), onlyXState.values.end());
  EXPECT_EQ(onlyXState.values.at(X).constant, 7);
  EXPECT_EQ(onlyXState.values.find(Y), onlyXState.values.end());

  auto withoutY = npa::AffineRelationDomain::havoc(relation, Y);
  auto withoutYState = npa::materializeAffineExpressions(withoutY);
  ASSERT_NE(withoutYState.values.find(X), withoutYState.values.end());
  EXPECT_EQ(withoutYState.values.at(X).constant, 7);
  EXPECT_EQ(withoutYState.values.find(Y), withoutYState.values.end());
}

TEST(AffineRelationDomain, SizeCountsSatisfyingTwoVocabularySolutions) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i8 %x) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto *X = &*module->getFunction("f")->arg_begin();
  auto idSize =
      npa::AffineRelationDomain::size(npa::AffineRelationDomain::identity());
  EXPECT_EQ(idSize.getZExtValue(), 256u);

  auto fixed = npa::AffineRelationDomain::addPrecondition(
      npa::AffineRelationDomain::identity(), X, 42);
  auto fixedSize = npa::AffineRelationDomain::size(fixed);
  EXPECT_EQ(fixedSize.getZExtValue(), 1u);

  auto bottomSize =
      npa::AffineRelationDomain::size(npa::AffineRelationDomain::zero());
  EXPECT_EQ(bottomSize.getZExtValue(), 0u);
}

TEST(AffineRelationDomain, MergePreservingLocalsKeepsCallerLocalValue) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %g, i32 %l) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  npa::AffineRelationDomain::configure(&vocab);

  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *ArgIt = F->arg_begin();
  auto *G = &*ArgIt;
  ++ArgIt;
  auto *L = &*ArgIt;

  auto callSite = npa::AffineRelationDomain::extend(
      npa::AffineRelationDomain::makeAffineAssignment(L, 2, {{L, 1}}),
      npa::AffineRelationDomain::makeAffineAssignment(G, 1, {{G, 1}}));
  auto calleeExit = npa::AffineRelationDomain::extend(
      npa::AffineRelationDomain::makeAffineAssignment(L, 0, {{G, 1}}),
      npa::AffineRelationDomain::makeAffineAssignment(G, 3, {{G, 1}}));

  auto merged = npa::AffineRelationDomain::mergePreservingLocals(
      callSite, calleeExit, {L});
  auto state = npa::materializeAffineExpressions(merged);

  ASSERT_NE(state.values.find(G), state.values.end());
  EXPECT_EQ(state.values.at(G).constant, 4);
  ASSERT_EQ(state.values.at(G).terms.size(), 1u);
  EXPECT_EQ(state.values.at(G).terms.at(G), 1);

  ASSERT_NE(state.values.find(L), state.values.end());
  EXPECT_EQ(state.values.at(L).constant, 2);
  ASSERT_EQ(state.values.at(L).terms.size(), 1u);
  EXPECT_EQ(state.values.at(L).terms.at(L), 1);
}

TEST(AffineRelationDomain, GenericProjectDropsConfiguredLocals) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %g, i32 %l) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *ArgIt = F->arg_begin();
  auto *G = &*ArgIt;
  ++ArgIt;
  auto *L = &*ArgIt;
  vocab.localValues = {L};
  npa::AffineRelationDomain::configure(&vocab);

  auto relation = npa::AffineRelationDomain::addPrecondition(
      npa::AffineRelationDomain::identity(), G, 7);
  relation = npa::AffineRelationDomain::addPrecondition(relation, L, 11);

  auto projected = npa::domain_project<npa::AffineRelationDomain>(relation);
  auto state = npa::materializeAffineExpressions(projected);

  ASSERT_NE(state.values.find(G), state.values.end());
  EXPECT_EQ(state.values.at(G).constant, 7);
  EXPECT_EQ(state.values.find(L), state.values.end());
}

TEST(AffineRelationDomain, GenericProjectIsIdempotentAndOptional) {
  llvm::LLVMContext ctx;
  auto module = parseModule(ctx, R"(
    define void @f(i32 %g, i32 %l) {
    entry:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto vocab = buildVocabulary(*module);
  auto *F = module->getFunction("f");
  ASSERT_NE(F, nullptr);
  auto *ArgIt = F->arg_begin();
  auto *G = &*ArgIt;
  ++ArgIt;
  auto *L = &*ArgIt;
  vocab.localValues = {L};
  npa::AffineRelationDomain::configure(&vocab);

  auto first = npa::AffineRelationDomain::addPrecondition(
      npa::AffineRelationDomain::identity(), G, 3);
  first = npa::AffineRelationDomain::addPrecondition(first, L, 5);

  auto second = npa::AffineRelationDomain::addPrecondition(
      npa::AffineRelationDomain::identity(), G, 9);
  second = npa::AffineRelationDomain::addPrecondition(second, L, 13);

  auto projectedFirst = npa::AffineRelationDomain::project(first);
  EXPECT_TRUE(npa::AffineRelationDomain::equal(
      npa::AffineRelationDomain::project(projectedFirst), projectedFirst));

  vocab.localValues.clear();
  npa::AffineRelationDomain::configure(&vocab);
  EXPECT_TRUE(npa::AffineRelationDomain::equal(
      npa::AffineRelationDomain::project(second), second));
}
