#pragma once

#include "IR/UseTraceSSA/Query.h"
#include <llvm/Support/CommandLine.h>
#include <ostream>

namespace lotus {
namespace usetracessa {
namespace cli {

struct QueryOptions {
  ContextMode mode = ContextMode::Realizable;
  std::optional<std::size_t> depth = DEFAULT_CONTEXT_LIMIT;
  SearchLimits limits;
  void apply(Query &query) const {
    query.context = mode; query.contextLimit = depth; limits.apply(query);
  }
  const char *contextName() const {
    return mode == ContextMode::Insensitive ? "insensitive" : "sensitive";
  }
  std::string depthName() const {
    return depth ? std::to_string(*depth) : "unlimited";
  }
};

class QueryOptionsParser {
  llvm::cl::opt<std::string> Context{"context", llvm::cl::init("sensitive"),
      llvm::cl::desc("Call/return matching: sensitive (default) or insensitive")};
  llvm::cl::opt<std::string> Depth{"context-limit",
      llvm::cl::init(std::to_string(DEFAULT_CONTEXT_LIMIT)),
      llvm::cl::desc("Call-string depth: N or unlimited (default: 3; 0: legacy merging; "
                     "used with sensitive context)")};
  llvm::cl::alias DepthAlias{"context-depth", llvm::cl::aliasopt(Depth),
      llvm::cl::desc("Alias for --context-limit")};
  llvm::cl::opt<bool> Unbounded{"unbounded-context", llvm::cl::init(false),
      llvm::cl::desc("Alias for --context-limit=unlimited")};
  llvm::cl::opt<std::string> Products{"max-product-states", llvm::cl::init("unlimited"),
      llvm::cl::desc("Per-query product-state budget: N or unlimited (0 is an alias)")};
  llvm::cl::opt<std::string> Summaries{"max-summary-pairs", llvm::cl::init("unlimited"),
      llvm::cl::desc("Per-query summary/context-state budget: N or unlimited")};

  static bool budget(const llvm::cl::opt<std::string> &option, std::size_t &value,
                     std::string &error) {
    llvm::StringRef text(option.getValue());
    if (text == "unlimited") { value = 0; return true; }
    if (text.empty() || text.find_first_not_of("0123456789") != llvm::StringRef::npos ||
        text.getAsInteger(10, value)) {
      error = "--" + option.ArgStr.str() + " expects a nonnegative integer or unlimited";
      return false;
    }
    return true;
  }

public:
  bool resolve(QueryOptions &options, std::string &error) const {
    const auto &mode = Context.getValue();
    if (mode != "sensitive" && mode != "insensitive") {
      error = "--context must be sensitive or insensitive"; return false;
    }
    if (Unbounded && (Depth.getNumOccurrences() || DepthAlias.getNumOccurrences()) &&
        Depth.getValue() != "unlimited") {
      error = "--unbounded-context conflicts with --context-limit/--context-depth"; return false;
    }
    options.mode = mode == "insensitive" ? ContextMode::Insensitive : ContextMode::Realizable;
    if (Unbounded || Depth.getValue() == "unlimited") {
      options.depth.reset();
    } else {
      std::size_t depth;
      if (!budget(Depth, depth, error)) return false;
      options.depth = depth;
    }
    return budget(Products, options.limits.maxProductStates, error) &&
           budget(Summaries, options.limits.maxSummaryPairs, error);
  }
};

inline void printCompletion(std::ostream &out, const SearchCompletion &completion) {
  out << " search_complete=" << (completion.searchComplete ? "true" : "false")
      << " model_complete=" << (completion.modelComplete ? "true" : "false")
      << " stop_reason=" << searchStopReasonName(completion.stopReason)
      << " budget_limit=" << completion.budgetLimit
      << " budget_observed=" << completion.budgetObserved;
}

inline void explainIncomplete(std::ostream &out, const SearchCompletion &completion) {
  if (completion.searchComplete) return;
  out << "Search incomplete: " << searchStopReasonName(completion.stopReason);
  if (completion.budgetLimit)
    out << " reached " << completion.budgetObserved << " (limit " << completion.budgetLimit
        << "). Increase the limit or rerun with --"
        << searchStopReasonName(completion.stopReason) << "=unlimited";
  out << ". Existing findings remain valid; enumeration is incomplete.\n";
}

} // namespace cli
} // namespace usetracessa
} // namespace lotus
