#pragma once

#include "Dataflow/APA/Core/Timing.h"
#include "Dataflow/APA/Solver/Ordering/Policy.h"

#include <cmath>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>
#include <tuple>

namespace elimination {
namespace order {

// LLVM-free versioned selector. A host supplies read-only local signal lookup,
// marks changed endpoints dirty, and detaches the selected vertex itself.
class OnlineOrderSelector final {
public:
  OnlineOrderSelector(OrderingPolicy Policy, const OrderPolicyOptions &Opts,
                      std::vector<std::size_t> Ranks, OrderingDiagnostics &Diag,
                      std::vector<std::size_t> NodeIds = {})
      : Policy(Policy), Opts(Opts), Ranks(std::move(Ranks)), Diag(Diag),
        NodeIds(std::move(NodeIds)), Live(this->Ranks.size(), true),
        Versions(this->Ranks.size(), 0), Current(this->Ranks.size()),
        Remaining(this->Ranks.size()), Priorities(this->Ranks.size(), 0),
        TieRanks(this->Ranks) {
    validateOrderOptions(Opts, Policy, Live.size());
    if (this->NodeIds.empty()) {
      this->NodeIds.resize(Live.size());
      std::iota(this->NodeIds.begin(), this->NodeIds.end(), 0);
    }
    if (this->NodeIds.size() != Live.size()) {
      throw std::invalid_argument(
          "APA node identifiers must match the live graph");
    }
    for (std::size_t I = 0; I < Live.size(); ++I) {
      Dirty.insert(I);
    }
    if (Policy == OrderingPolicy::Explicit) {
      Priorities = ExplicitPolicy::priorities(Opts.ExplicitOrder);
    } else if (Policy == OrderingPolicy::Random) {
      Priorities = RandomPolicy::priorities(Live.size(), Opts.RandomSeed);
    }
  }

  void markDirty(std::size_t Node) {
    elimination::detail::ScopedNanoseconds Timer(Diag.selection_time_ns);
    if (Live.at(Node)) {
      Dirty.insert(Node);
    }
  }

  template <typename SignalFn, typename DegreeFn>
  std::size_t chooseNext(SignalFn ReadSignals, DegreeFn ReadDegree) {
    elimination::detail::ScopedNanoseconds SelectionTimer(
        Diag.selection_time_ns);
    if (Remaining == 0) {
      throw std::logic_error("APA selector has no remaining vertex");
    }
    // Min-fill depends on edges BETWEEN neighbors, so incident-only dirty
    // marking is insufficient. Full rescoring is mandatory for that baseline.
    if (!Opts.Incremental || Policy == OrderingPolicy::MinFill) {
      {
        elimination::detail::ScopedNanoseconds Timer(Diag.heap_time_ns);
        Heap = {};
      }
      for (std::size_t I = 0; I < Live.size(); ++I) {
        if (Live[I]) {
          Dirty.insert(I);
        }
      }
    }
    for (auto Node : Dirty) {
      if (!Live[Node]) {
        continue;
      }
      CandidateScoreSample Sample;
      Sample.step = Step;
      Sample.node = NodeIds[Node];
      Sample.version = ++Versions[Node];
      {
        elimination::detail::ScopedNanoseconds Timer(Diag.scoring_time_ns);
        Sample.signals = ReadSignals(Node);
        const auto Degree = ReadDegree(Node);
        Sample.score = policyScore(Policy, Sample.signals, Opts, Degree,
                                   Ranks[Node], Priorities[Node]);
        TieRanks[Node] = Ranks[Node];
        if (Policy == OrderingPolicy::MinFill) {
          TieRanks[Node] = Degree;
        }
      }
      ++Diag.score_refreshes;
      Current[Node] = Sample;
      if (Opts.RecordTrace) {
        Diag.score_updates.push_back(Sample);
      }
      elimination::detail::ScopedNanoseconds Timer(Diag.heap_time_ns);
      Heap.push(Entry{Sample.score, TieRanks[Node], NodeIds[Node], Node,
                      Versions[Node]});
    }
    Dirty.clear();
    elimination::detail::ScopedNanoseconds Timer(Diag.heap_time_ns);
    // Bound retained obsolete entries; lazy deletion alone can retain O(N^2).
    if (Heap.size() > 4 * Remaining + 64) {
      ++Diag.heap_compactions;
      Heap = {};
      for (std::size_t I = 0; I < Live.size(); ++I) {
        if (Live[I]) {
          Heap.push(
              Entry{Current[I].score, TieRanks[I], NodeIds[I], I, Versions[I]});
        }
      }
    }
    while (!Heap.empty()) {
      const auto Top = Heap.top();
      Heap.pop();
      if (!Live[Top.node] || Top.version != Versions[Top.node]) {
        ++Diag.stale_heap_entries;
        continue;
      }
      Live[Top.node] = false;
      --Remaining;
      ++Step;
      ++Diag.selected_nodes;
      return Top.node;
    }
    throw std::logic_error("APA selector lost a live candidate");
  }

  const CandidateScoreSample &selectedScore(std::size_t Node) const {
    return Current.at(Node);
  }

private:
  struct Entry final {
    double score;
    std::size_t rank, id, node, version;
    bool operator<(const Entry &Other) const {
      return std::tie(score, rank, id, version) >
             std::tie(Other.score, Other.rank, Other.id, Other.version);
    }
  };
  OrderingPolicy Policy;
  OrderPolicyOptions Opts;
  std::vector<std::size_t> Ranks;
  OrderingDiagnostics &Diag;
  std::vector<std::size_t> NodeIds;
  std::vector<bool> Live;
  std::vector<std::size_t> Versions;
  std::vector<CandidateScoreSample> Current;
  std::size_t Remaining, Step = 0;
  std::vector<std::size_t> Priorities;
  std::vector<std::size_t> TieRanks;
  std::set<std::size_t> Dirty;
  std::priority_queue<Entry> Heap;
};

} // namespace order
} // namespace elimination
