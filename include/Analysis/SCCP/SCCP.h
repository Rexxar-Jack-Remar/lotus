/** @file SCCP.h @brief Sparse Conditional Constant Propagation analysis. */
#pragma once

#include <deque>
#include <set>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

namespace lotus {
namespace analysis {
namespace sccp {

enum class SccpValueKind {
  Top,
  Constant,
  Bottom,
};

class SccpValue {
public:
  SccpValue() = default;

  static SccpValue getTop(void);
  static SccpValue getConstant(const llvm::ConstantInt *constant);
  static SccpValue getBottom(void);

  SccpValueKind getKind(void) const;
  bool isTop(void) const;
  bool isConstant(void) const;
  bool isBottom(void) const;
  const llvm::ConstantInt *getConstant(void) const;

  SccpValue meet(const SccpValue &other) const;

  bool operator==(const SccpValue &other) const;
  bool operator!=(const SccpValue &other) const;

private:
  SccpValue(SccpValueKind kind, const llvm::ConstantInt *constant);

  SccpValueKind kind_ = SccpValueKind::Top;
  const llvm::ConstantInt *constant_ = nullptr;
};

struct FunctionResult {
  llvm::MapVector<const llvm::Value *, const llvm::ConstantInt *> constants;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> dead_blocks;
};

struct ModuleResult {
  llvm::MapVector<const llvm::Function *, FunctionResult> function_results;
  llvm::MapVector<const llvm::Value *, const llvm::ConstantInt *> constants;
  llvm::SmallPtrSet<const llvm::BasicBlock *, 16> dead_blocks;
};

FunctionResult runSCCPOnFunction(const llvm::Function &function);
ModuleResult runSCCPOnModule(llvm::Module &module);

} // namespace sccp
} // namespace analysis
} // namespace lotus
