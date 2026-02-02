/**
 * @file TPATest.cpp
 * @brief Unit tests for TPA (semi-sparse flow- and context-sensitive pointer analysis)
 */

#include "Alias/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/SemiSparseProgramBuilder.h"
#include "Alias/TPA/PointerAnalysis/Support/PtsSet.h"
#include "Alias/TPA/Transforms/RunPrepass.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace tpa;
using namespace transform;

namespace {

std::unique_ptr<Module> parseModule(LLVMContext &ctx, const char *ir) {
  SMDiagnostic err;
  auto M = parseAssemblyString(ir, err, ctx);
  if (!M)
    err.print("TPATest", errs());
  return M;
}

// Two pointers alias iff their points-to sets have a common object.
bool mayAlias(const SemiSparsePointerAnalysis &pta, const Value *v1, const Value *v2) {
  PtsSet pts1 = pta.getPtsSet(v1);
  PtsSet pts2 = pta.getPtsSet(v2);
  for (const auto *obj : pts1) {
    if (pts2.has(obj))
      return true;
  }
  return false;
}

} // namespace

class TPATest : public ::testing::Test {
protected:
  LLVMContext context;
};

TEST_F(TPATest, NoAliasTwoAllocas) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %y = alloca i32
      ret i32 0
    }
  )";

  auto module = parseModule(context, ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *x = nullptr, *y = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (!x) x = AI;
        else if (!y) y = AI;
      }
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(y, nullptr);

  EXPECT_FALSE(mayAlias(pta, x, y));
}

TEST_F(TPATest, AliasStoreLoad) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(context, ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *x = nullptr, *q = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getAllocatedType()->isIntegerTy(32))
          x = AI;
      }
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy())
          q = LI;
      }
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(q, nullptr);

  EXPECT_TRUE(mayAlias(pta, x, q));
}

TEST_F(TPATest, PointsToSetLoadContainsStoredAlloca) {
  const char *ir = R"(
    define i32 @main() {
      %x = alloca i32
      %p = alloca i32*
      store i32* %x, i32** %p
      %q = load i32*, i32** %p
      ret i32 0
    }
  )";

  auto module = parseModule(context, ir);
  ASSERT_NE(module, nullptr);

  runPrepassOn(*module);
  SemiSparseProgramBuilder builder;
  SemiSparseProgram ssProg = builder.runOnModule(*module);
  SemiSparsePointerAnalysis pta;
  pta.runOnProgram(ssProg);

  Function *mainFn = module->getFunction("main");
  ASSERT_NE(mainFn, nullptr);

  const Value *q = nullptr;
  for (auto &BB : *mainFn) {
    for (auto &I : BB) {
      if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
          q = LI;
          break;
        }
      }
    }
  }
  ASSERT_NE(q, nullptr);

  PtsSet ptsQ = pta.getPtsSet(q);
  EXPECT_FALSE(ptsQ.empty());
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
