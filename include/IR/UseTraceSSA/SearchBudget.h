#pragma once

#include <cstddef>

namespace lotus {
namespace usetracessa {

constexpr std::size_t DEFAULT_CONTEXT_LIMIT = 3;

struct Query;
struct SearchLimits {
  // Zero disables a resource limit. Context depth is a separate precision setting.
  std::size_t maxProductStates = 0;
  std::size_t maxSummaryPairs = 0;
  void apply(Query &query) const;
};

enum class SearchStopReason {
  None,
  ProductStates,
  SummaryPairs,
  WitnessUnavailable,
  FirstFinding
};

inline const char *searchStopReasonName(SearchStopReason reason) {
  switch (reason) {
  case SearchStopReason::None: return "none";
  case SearchStopReason::ProductStates: return "max-product-states";
  case SearchStopReason::SummaryPairs: return "max-summary-pairs";
  case SearchStopReason::WitnessUnavailable: return "witness-unavailable";
  case SearchStopReason::FirstFinding:
    return "first-finding";
  }
  return "invalid";
}

struct SearchCompletion {
  /// No search was interrupted. This does not claim the model is complete.
  bool searchComplete = true;
  bool modelComplete = true;
  /// First interruption, including its configured limit and observed counter.
  SearchStopReason stopReason = SearchStopReason::None;
  std::size_t budgetLimit = 0, budgetObserved = 0;

  bool complete() const { return searchComplete && modelComplete; }
  void stop(SearchStopReason reason, std::size_t limit = 0, std::size_t observed = 0) {
    searchComplete = false;
    if (stopReason == SearchStopReason::None) {
      stopReason = reason; budgetLimit = limit; budgetObserved = observed;
    }
  }
  void merge(const SearchCompletion &other) {
    modelComplete &= other.modelComplete;
    if (!other.searchComplete) stop(other.stopReason, other.budgetLimit, other.budgetObserved);
  }
};

namespace detail {
struct SearchBudgetExceeded {
  SearchStopReason reason;
  std::size_t limit, observed;
};
inline void checkSearchBudget(std::size_t count, std::size_t limit, SearchStopReason reason) {
  if (limit && count >= limit) throw SearchBudgetExceeded{reason, limit, count};
}
} // namespace detail
} // namespace usetracessa
} // namespace lotus
