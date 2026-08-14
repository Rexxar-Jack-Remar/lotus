#include "Checker/KINT/BugDetection.h"
#include "Checker/KINT/KINTTaintAnalysis.h"
#include "Checker/KINT/MKintPass.h"
#include "Checker/KINT/Options.h"
#include "Checker/KINT/SmtMemory.h"
#include "TestUtils/LLVMHelpers.h"

#include <optional>

#include <gtest/gtest.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <z3++.h>

using namespace llvm;

namespace {

uint64_t getNumeralU64(const z3::expr &expr) {
  z3::expr simplified = expr.simplify();
  uint64_t value = 0;
  EXPECT_TRUE(Z3_get_numeral_uint64(simplified.ctx(), simplified, &value));
  return value;
}

z3::expr bvValFromAPInt(z3::context &ctx, const llvm::APInt &value) {
  llvm::SmallString<64> decimal;
  value.toString(decimal, 10, /*Signed=*/false, /*formatAsCLiteral=*/false);
  Z3_sort sort = Z3_mk_bv_sort(ctx, value.getBitWidth());
  Z3_ast ast = Z3_mk_numeral(ctx, decimal.c_str(), sort);
  return z3::to_expr(ctx, ast);
}

class KINTCheckerTest : public ::testing::Test {
protected:
  LLVMContext context;

  std::unique_ptr<Module> parseModule(const char *source) {
    return lotus::unittest::parseModule(context, source, "KINTCheckerTest");
  }

  void runPass(Module &module) {
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    llvm::PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    kint::MKintPass pass;
    pass.run(module, MAM);
  }

  static bool hasKintErrorMetadata(const Function &F,
                                   llvm::Instruction::BinaryOps opcode) {
    for (const Instruction &I : instructions(F)) {
      const auto *bin = dyn_cast<BinaryOperator>(&I);
      if (!bin || bin->getOpcode() != opcode)
        continue;
      if (bin->getMetadata("mkint.err"))
        return true;
    }
    return false;
  }
};

#include "Fragments/KINTSmtMemory.inc"
#include "Fragments/KINTTaintAndReachability.inc"
#include "Fragments/KINTInterproceduralSummaries.inc"
