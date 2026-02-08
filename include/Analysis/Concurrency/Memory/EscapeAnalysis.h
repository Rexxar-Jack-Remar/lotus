#ifndef ESCAPE_ANALYSIS_H
#define ESCAPE_ANALYSIS_H

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Pass.h>
#include <unordered_map>
#include <unordered_set>

namespace lotus {

class EscapeAnalysis {
public:
  explicit EscapeAnalysis(llvm::Module &module);

  void analyze();

  /**
   * @brief Check if a value escapes the thread (is shared)
   * @param val The value to check
   * @return true if the value may escape to other threads
   */
  bool isEscaped(const llvm::Value *val) const;

  /**
   * @brief Check if a value is thread-local
   * @param val The value to check
   * @return true if the value is guaranteed to be thread-local
   */
  bool isThreadLocal(const llvm::Value *val) const;

private:
  llvm::Module &m_module;
  std::unordered_set<const llvm::Value *> m_escaped_values;
  
  // Cache for visited values during traversal
  mutable std::unordered_set<const llvm::Value *> m_visited;

  void runEscapeAnalysis();
  void propagateEscape(const llvm::Value *val);
  bool isGlobalOrArgument(const llvm::Value *val) const;
};

} // namespace lotus

#endif // ESCAPE_ANALYSIS_H

