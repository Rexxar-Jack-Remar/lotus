//===-- SifaDagInterpreterTest.cpp - Full Sifa DAG interpreter pipeline ----===//
//
// Exercises ProcedureResources -> DagInterpreter -> FixpointLoopSummarizer
// with ReachabilityDomain (same pipeline as isReachable()).
//
//===----------------------------------------------------------------------===//

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/ReachabilityDomain.h"
#include "Verification/Sifa/Fluid/NeverFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

#include <memory>

namespace {

static const llvm::BasicBlock *getBlockByName(const llvm::Function &F, const char *name) {
  for (const llvm::BasicBlock &BB : F) {
    if (BB.getName() == name) {
      return &BB;
    }
  }
  return nullptr;
}

TEST(SifaDagInterpreter, FullPipelineReachableAndUnreachable) {
  const char *ir = R"IR(
    define i32 @f(i32 %n) {
    entry:
      br label %loop

    loop:
      %i = phi i32 [ 0, %entry ], [ %inc, %body ]
      %cmp = icmp slt i32 %i, %n
      br i1 %cmp, label %body, label %exit

    body:
      %inc = add i32 %i, 1
      br label %loop

    exit:
      ret i32 0

    unreach:
      ret i32 1
    }
  )IR";

  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(ir, err, ctx);
  ASSERT_NE(M, nullptr);

  const llvm::Function *F = M->getFunction("f");
  ASSERT_NE(F, nullptr);

  const llvm::BasicBlock *body = getBlockByName(*F, "body");
  const llvm::BasicBlock *exit = getBlockByName(*F, "exit");
  const llvm::BasicBlock *unreach = getBlockByName(*F, "unreach");
  ASSERT_NE(body, nullptr);
  ASSERT_NE(exit, nullptr);
  ASSERT_NE(unreach, nullptr);

  lotus::sifa::SifaStats stats;
  lotus::sifa::ReachabilityDomain<lotus::sifa::Transition> domain;
  lotus::sifa::NeverFluid<bool> fluid;

  lotus::sifa::DagInterpreter<lotus::sifa::Transition, bool> ipr(stats, domain, fluid);
  lotus::sifa::FixpointLoopSummarizer<lotus::sifa::Transition, bool> loopSum(stats, domain, fluid, ipr);
  ipr.setLoopSummarizer(loopSum);

  lotus::sifa::ProcedureResources res(
      stats, *F,
      {const_cast<llvm::BasicBlock *>(body)});

  bool bodyReachable =
      ipr.interpretForSingleMarker(res.getRegexDag(), res.getDagOverlayPathToLois(), /*initialInput=*/true);
  EXPECT_TRUE(bodyReachable);

  lotus::sifa::ProcedureResources resExit(stats, *F,
                                          {const_cast<llvm::BasicBlock *>(exit)});
  bool exitReachable =
      ipr.interpretForSingleMarker(resExit.getRegexDag(), resExit.getDagOverlayPathToLois(), true);
  EXPECT_TRUE(exitReachable);

  lotus::sifa::ProcedureResources resUnreach(stats, *F,
                                            {const_cast<llvm::BasicBlock *>(unreach)});
  bool unreachReachable =
      ipr.interpretForSingleMarker(resUnreach.getRegexDag(), resUnreach.getDagOverlayPathToLois(), true);
  EXPECT_FALSE(unreachReachable);
}

} // namespace
