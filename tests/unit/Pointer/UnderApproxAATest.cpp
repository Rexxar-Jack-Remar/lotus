/**
 * @file UnderApproxAATest.cpp
 * @brief Unit tests for UnderApproxAA/EquivDB must-alias inference.
 */

#include "Alias/UnderApproxAA/EquivDB.h"
#include "Alias/UnderApproxAA/UnderApproxAA.h"

#include <memory>

#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace UnderApprox;

namespace {

struct MemorySSAContext {
  TargetLibraryInfoImpl TLII;
  TargetLibraryInfo TLI;
  AssumptionCache AC;
  AAResults AAR;
  BasicAAResult BAA;
  std::unique_ptr<MemorySSA> MSSA;

  MemorySSAContext(Function &F, DominatorTree &DT)
      : TLI(TLII), AC(F), AAR(TLI),
        BAA(F.getParent()->getDataLayout(), F, TLI, AC, &DT) {
    AAR.addAAResult(BAA);
    MSSA = std::make_unique<MemorySSA>(F, &AAR, &DT);
  }
};

class UnderApproxAATest : public ::testing::Test {
protected:
  LLVMContext Ctx;

  std::unique_ptr<Module> parseModule(const char *Source) {
    SMDiagnostic Err;
    auto M = parseAssemblyString(Source, Err, Ctx);
    if (!M)
      Err.print("UnderApproxAATest", errs());
    return M;
  }

  Instruction *findInst(Function *F, StringRef Name) {
    for (BasicBlock &BB : *F)
      for (Instruction &I : BB)
        if (I.hasName() && I.getName() == Name)
          return &I;
    return nullptr;
  }
};

TEST_F(UnderApproxAATest, ConstantSelectMustAliasChosenArm) {
  const char *Source = R"(
    define void @test(i8* %p, i8* %q) {
    entry:
      %s = select i1 true, i8* %p, i8* %q
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  Instruction *S = findInst(F, "s");
  Argument *P = F->getArg(0);
  ASSERT_NE(S, nullptr);
  ASSERT_NE(P, nullptr);

  EquivDB DB(*F);
  EXPECT_TRUE(DB.mustAlias(S, P));
}

TEST_F(UnderApproxAATest, MemoryPhiForwardingUsesMustAliasValueClass) {
  const char *Source = R"(
    define i8* @test(i1 %c1, i1 %c2) {
    entry:
      %x = alloca i8
      %slot = alloca i8*
      %x_cast = bitcast i8* %x to i8*
      br i1 %c1, label %then, label %else

    then:
      store i8* %x_cast, i8** %slot
      br label %merge

    else:
      %v_alt = select i1 %c2, i8* %x, i8* %x_cast
      store i8* %v_alt, i8** %slot
      br label %merge

    merge:
      %q = load i8*, i8** %slot
      ret i8* %q
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *X = findInst(F, "x");
  auto *Q = findInst(F, "q");
  ASSERT_NE(X, nullptr);
  ASSERT_NE(Q, nullptr);

  DominatorTree DT(*F);
  MemorySSAContext MemCtx(*F, DT);

  EquivDB DB(*F, MemCtx.MSSA.get(), &DT);
  EXPECT_TRUE(DB.mustAlias(Q, X));
}

TEST_F(UnderApproxAATest, SingleStoreAllocaIsSlotSensitive) {
  const char *Source = R"(
    define void @test(i8* %a, i8* %b) {
    entry:
      %slot = alloca [2 x i8*]
      %p0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %slot, i64 0, i64 0
      %p1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %slot, i64 0, i64 1
      store i8* %a, i8** %p0
      store i8* %b, i8** %p1
      %l0 = load i8*, i8** %p0
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *A = F->getArg(0);
  auto *L0 = findInst(F, "l0");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(L0, nullptr);

  DominatorTree DT(*F);
  EquivDB DB(*F, nullptr, &DT);
  EXPECT_TRUE(DB.mustAlias(L0, A));
}

TEST_F(UnderApproxAATest, SingleStoreGlobalSlotIsForwarded) {
  const char *Source = R"(
    @G = global [2 x i8*] zeroinitializer

    define void @test(i8* %a, i8* %b) {
    entry:
      %g0 = getelementptr inbounds [2 x i8*], [2 x i8*]* @G, i64 0, i64 0
      %g1 = getelementptr inbounds [2 x i8*], [2 x i8*]* @G, i64 0, i64 1
      %g0_alias = bitcast i8** %g0 to i8**
      store i8* %a, i8** %g0
      store i8* %b, i8** %g1
      %l0 = load i8*, i8** %g0_alias
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *A = F->getArg(0);
  auto *L0 = findInst(F, "l0");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(L0, nullptr);

  DominatorTree DT(*F);
  EquivDB DB(*F, nullptr, &DT);
  EXPECT_TRUE(DB.mustAlias(L0, A));
}

TEST_F(UnderApproxAATest, SingleStoreHeapSlotIsForwarded) {
  const char *Source = R"(
    declare noalias i8* @malloc(i64)

    define void @test(i8* %a, i8* %b) {
    entry:
      %buf = call i8* @malloc(i64 16)
      %slot0 = bitcast i8* %buf to i8**
      %slot1 = getelementptr inbounds i8*, i8** %slot0, i64 1
      %slot0_alias = bitcast i8** %slot0 to i8**
      store i8* %a, i8** %slot0
      store i8* %b, i8** %slot1
      %l0 = load i8*, i8** %slot0_alias
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *A = F->getArg(0);
  auto *L0 = findInst(F, "l0");
  ASSERT_NE(A, nullptr);
  ASSERT_NE(L0, nullptr);

  DominatorTree DT(*F);
  EquivDB DB(*F, nullptr, &DT);
  EXPECT_TRUE(DB.mustAlias(L0, A));
}

TEST_F(UnderApproxAATest, UnderApproxAAHandlesGlobalVsLocalFunctionQuery) {
  const char *Source = R"(
    @G = global i8 0

    define i8* @test() {
    entry:
      %p = getelementptr inbounds i8, i8* @G, i64 0
      ret i8* %p
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  auto *G = M->getNamedGlobal("G");
  auto *F = M->getFunction("test");
  ASSERT_NE(G, nullptr);
  ASSERT_NE(F, nullptr);

  auto *P = findInst(F, "p");
  ASSERT_NE(P, nullptr);

  UnderApproxAA AA(*M);
  EXPECT_TRUE(AA.mustAlias(P, G));
}

TEST_F(UnderApproxAATest, ClosedGEPSupportsEquivalentIntegerIndexExprs) {
  const char *Source = R"(
    define void @test(i8* %base, i64 %i) {
    entry:
      %base1 = bitcast i8* %base to i8*
      %base2 = select i1 true, i8* %base, i8* %base
      %idx1 = add i64 %i, 1
      %idx2 = add i64 1, %i
      %p = getelementptr inbounds i8, i8* %base1, i64 %idx1
      %q = getelementptr inbounds i8, i8* %base2, i64 %idx2
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *P = findInst(F, "p");
  auto *Q = findInst(F, "q");
  ASSERT_NE(P, nullptr);
  ASSERT_NE(Q, nullptr);

  EquivDB DB(*F);
  EXPECT_TRUE(DB.mustAlias(P, Q));
}

TEST_F(UnderApproxAATest, UnderApproxAACacheRefreshesAfterIRMutation) {
  const char *Source = R"(
    define void @test(i8* %p, i8* %q) {
    entry:
      %s = select i1 true, i8* %p, i8* %q
      ret void
    }
  )";

  auto M = parseModule(Source);
  ASSERT_NE(M, nullptr);

  Function *F = M->getFunction("test");
  ASSERT_NE(F, nullptr);

  auto *P = F->getArg(0);
  auto *Q = F->getArg(1);
  auto *S = dyn_cast<SelectInst>(findInst(F, "s"));
  ASSERT_NE(P, nullptr);
  ASSERT_NE(Q, nullptr);
  ASSERT_NE(S, nullptr);

  UnderApproxAA AA(*M);
  EXPECT_FALSE(AA.mustAlias(S, Q));

  S->setCondition(ConstantInt::getTrue(Ctx));
  S->setTrueValue(P);
  S->setFalseValue(P);

  EXPECT_TRUE(AA.mustAlias(S, P));
}

} // namespace
