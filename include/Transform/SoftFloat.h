/**
 * \file SoftFloat.h
 * \brief Pass for replacing floating-point operations with software emulation
 * \author Lotus Team
 *
 * This pass replaces floating-point instructions with calls to libgcc softfloat
 * library functions, enabling floating-point operations on targets without
 * native FPU support.
 */
#pragma once

#include <llvm/IR/Function.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

/**
 * \class SoftFloatPass
 * \brief Soft-float transformation pass for software floating-point emulation
 *
 * This pass replaces all floating-point instructions with calls to the
 * corresponding libgcc softfloat library functions. This is essential for
 * targets that lack native floating-point hardware support.
 *
 * The pass handles:
 * - Floating-point arithmetic operations (fadd, fsub, fmul, fdiv, fneg)
 * - Floating-point conversions (fptosi, fptoui, sitofp, uitofp, fpext, fptrunc)
 * - Floating-point comparisons (fcmp)
 *
 * After this pass, the IR contains no floating-point operations, making it
 * suitable for soft-float targets.
 */
class SoftFloatPass : public PassInfoMixin<SoftFloatPass> {
public:
  /**
   * \brief Run the soft-float transformation on a function
   * \param F The function to transform
   * \param FAM Function analysis manager
   * \return PreservedAnalyses indicating whether the module was modified
   */
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
};

