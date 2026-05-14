#include "Checker/KINT/SummaryEncoding.h"

namespace kint {

SummaryObjectBinding::SummaryObjectBinding(
    SummaryObjectKind kind, const llvm::Value *root, unsigned arg_index,
    bool include_in_frame, const std::optional<z3::expr> &base_in_symbol,
    const std::optional<z3::expr> &size_in_symbol,
    const std::optional<z3::expr> &mem_in,
    const std::optional<z3::expr> &base_out_symbol,
    const std::optional<z3::expr> &size_out_symbol,
    const std::optional<z3::expr> &mem_out)
    : kind(kind), root(root), arg_index(arg_index),
      include_in_frame(include_in_frame), base_in_symbol(base_in_symbol),
      size_in_symbol(size_in_symbol), mem_in(mem_in),
      base_out_symbol(base_out_symbol), size_out_symbol(size_out_symbol),
      mem_out(mem_out) {}

FunctionSummary::FunctionSummary(const llvm::Function *function)
    : function(function) {}

SummaryCacheEntry::SummaryCacheEntry(const llvm::Function *function)
    : summary(function) {}

} // namespace kint
