#pragma once

#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/IR/Function.h>
#include <z3++.h>

namespace kint {

enum class SummaryAvailability {
  Disabled,
  Available,
  Unsupported,
};

enum class SummaryObjectKind {
  Argument,
  Global,
  EscapedReturn,
};

struct SummaryObjectBinding {
  SummaryObjectKind kind = SummaryObjectKind::Argument;
  const llvm::Value *root = nullptr;
  unsigned arg_index = ~0U;
  bool include_in_frame = true;
  std::optional<z3::expr> base_in_symbol;
  std::optional<z3::expr> size_in_symbol;
  std::optional<z3::expr> mem_in;
  std::optional<z3::expr> base_out_symbol;
  std::optional<z3::expr> size_out_symbol;
  std::optional<z3::expr> mem_out;

  SummaryObjectBinding(SummaryObjectKind kind, const llvm::Value *root,
                       unsigned arg_index, bool include_in_frame,
                       const std::optional<z3::expr> &base_in_symbol,
                       const std::optional<z3::expr> &size_in_symbol,
                       const std::optional<z3::expr> &mem_in,
                       const std::optional<z3::expr> &base_out_symbol,
                       const std::optional<z3::expr> &size_out_symbol,
                       const std::optional<z3::expr> &mem_out);
};

struct FunctionSummary {
  const llvm::Function *function = nullptr;
  SummaryAvailability availability = SummaryAvailability::Unsupported;
  bool recursive = false;
  bool has_integer_return = false;
  bool has_pointer_return = false;
  const llvm::Value *pointer_return_root = nullptr;
  std::optional<z3::expr> integer_return_symbol;
  std::optional<z3::expr> pointer_return_symbol;
  std::vector<const llvm::Argument *> integer_args;
  std::vector<const llvm::Argument *> pointer_args;
  llvm::DenseMap<const llvm::Value *, std::optional<z3::expr>> arg_symbols;
  std::vector<SummaryObjectBinding> boundary_objects;
  llvm::DenseSet<const llvm::Value *> modified_objects;
  std::vector<z3::expr> entry_constraints;
  std::vector<z3::expr> exit_constraints;
  std::vector<z3::expr> frame_constraints;
  std::vector<z3::expr> allocation_constraints;
  std::vector<z3::expr> path_case_clauses;
  std::string unsupported_reason;

  FunctionSummary() = default;
  explicit FunctionSummary(const llvm::Function *function);
};

struct SummaryCacheEntry {
  FunctionSummary summary;
  bool building = false;

  SummaryCacheEntry() = default;
  explicit SummaryCacheEntry(const llvm::Function *function);
};

} // namespace kint
