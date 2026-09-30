#include "KINTCheckerFixture.h"

#include "Checker/KINT/MKintPass.h"

#include <llvm/Passes/PassBuilder.h>

namespace kint_test {
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
} // namespace kint_test

void KINTCheckerTest::runPass(llvm::Module &module) {
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
