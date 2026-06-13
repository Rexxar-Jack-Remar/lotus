/**
 * @file TypeHierarchyAnalysis.h
 * @brief Type Hierarchy Analysis for C++ programs
 *
 * This analysis performs class hierarchy analysis for C++ programs,
 * including virtual call resolution and vtable reconstruction.
 *
 * @author Lotus Analysis Framework
 */

#pragma once

#include "llvm/ADT/SmallVector.h"

#include <memory>

namespace llvm {
class Module;
class Function;
class CallBase;
class raw_ostream;
} // namespace llvm

namespace lotus {

class TypeHierarchyAnalysis_Impl;

/// @brief Type Hierarchy Analysis implementation for C++ programs
///
/// Provides virtual call resolution and vtable reconstruction through
/// class hierarchy analysis.
class TypeHierarchyAnalysis {
public:
  using function_vector_t = llvm::SmallVector<const llvm::Function *, 16>;

  TypeHierarchyAnalysis(llvm::Module &M);

  ~TypeHierarchyAnalysis();

  /// @brief Build the class hierarchy graph and reconstruct vtables
  void calculate(void);

  /// @brief Check if a virtual call has been resolved
  /// @param CS The callsite to check
  /// @return true if the callsite is a resolved virtual call
  bool isVCallResolved(const llvm::CallBase &CS) const;

  /// @brief Get all possible callees for a C++ virtual call
  /// @param CS The virtual callsite
  /// @return Vector of possible callee functions, empty if not a virtual call
  const function_vector_t &getVCallCallees(const llvm::CallBase &CS);

  /// @brief Print the class hierarchy graph
  /// @param o Output stream for printing
  void printClassHierarchy(llvm::raw_ostream &o) const;

  /// @brief Print vtables for each class
  /// @param o Output stream for printing
  void printVtables(llvm::raw_ostream &o) const;

  /// @brief Print analysis statistics
  /// @param o Output stream for printing
  void printStats(llvm::raw_ostream &o) const;

private:
  std::unique_ptr<TypeHierarchyAnalysis_Impl>
      m_cha_impl; ///< Internal implementation
};

} // namespace lotus
