/**
 * \file LowerGlobalConstantArraySelect.h
 * \brief Pass for lowering select operations on global constant arrays
 * \author Lotus Team
 *
 * This pass transforms select operations that operate on global constant
 * arrays into function calls, enabling more efficient handling of constant
 * array selection patterns.
 */
#ifndef TRANSFORM_LOWERGLOBALCONSTANTARRAYSELECT_H
#define TRANSFORM_LOWERGLOBALCONSTANTARRAYSELECT_H

#include <map>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

/**
 * \class LowerGlobalConstantArraySelect
 * \brief Global constant array select lowering pass
 *
 * This pass identifies select operations that operate on global constant
 * arrays and replaces them with function calls. This can improve:
 * - Code generation efficiency
 * - Memory access patterns
 * - Analysis accuracy for constant data
 */
class LowerGlobalConstantArraySelect
    : public PassInfoMixin<LowerGlobalConstantArraySelect> {
private:
  /// Map of value to selection function
  std::map<Value *, Function *> SelectFuncMap;

public:
  /**
   * \brief Default virtual destructor
   */
  ~LowerGlobalConstantArraySelect() = default;

  /**
   * \brief Run the pass on a module
   * \param M The module to transform
   * \return true if the module was modified
   */
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);

private:
  /**
   * \brief Check if an instruction is a select on a global constant array
   * \param I The instruction to check
   * \return true if the instruction matches the pattern
   */
  bool isSelectGlobalConstantArray(Instruction &I);

  /**
   * \brief Initialize the pass for a function with constant data
   * \param F The function to initialize
   * \param CDA The constant data array
   */
  void initialize(Function *F, ConstantDataArray *CDA);
};

#endif // TRANSFORM_LOWERGLOBALCONSTANTARRAYSELECT_H
