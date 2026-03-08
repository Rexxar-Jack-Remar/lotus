#include "Dataflow/NPA/Analyses/Interprocedural/InterproceduralAffineEqualities.h"
#include "Dataflow/NPA/Domains/AffineRelationDomain.h"

#include <gtest/gtest.h>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>

#include <set>

namespace {

std::unique_ptr<llvm::Module> parseModule(llvm::LLVMContext &ctx,
                                          const char *ir) {
  llvm::SMDiagnostic err;
  auto module = llvm::parseAssemblyString(ir, err, ctx);
  if (!module)
    err.print("AffineRelationDomainTest", llvm::errs());
  return module;
}

npa::AffineRelationVocabulary buildVocabulary(const llvm::Module &M) {
  npa::AffineRelationVocabulary vocab;
  std::set<const llvm::Value *> ordered;
  for (const auto &F : M) {
    if (F.isDeclaration())
      continue;
    for (const auto &Arg : F.args()) {
      if (Arg.getType()->isIntegerTy() && Arg.getType()->getIntegerBitWidth() <= 64)
        ordered.insert(&Arg);
    }
    for (const auto &BB : F) {
      for (const auto &I : BB) {
        if (I.getType()->isIntegerTy() && I.getType()->getIntegerBitWidth() <= 64)
          ordered.insert(&I);
      }
    }
  }
  vocab.values.assign(ordered.begin(), ordered.end());
  for (unsigned i = 0; i < vocab.values.size(); ++i) {
    vocab.indices[vocab.values[i]] = i;
    vocab.actualBitWidths[vocab.values[i]] =
        vocab.values[i]->getType()->getIntegerBitWidth();
  }
  return vocab;
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
  auto composed =
      npa::AffineRelationDomain::extend(assign, npa::AffineRelationDomain::identity());
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
}
