#pragma once

#include "Dataflow/VASCO/Core/DirectedGraph.h"
#include "Dataflow/VASCO/Support/PseudoTopologicalOrder.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace vasco {

template <typename N> struct WorkItemComparator {
  using OrderMap = std::map<N, std::size_t>;

  std::shared_ptr<const OrderMap> Numbers = std::make_shared<const OrderMap>();

  bool operator()(const std::optional<N> &LHS,
                  const std::optional<N> &RHS) const {
    const auto RankOf = [&](const std::optional<N> &Item) -> std::size_t {
      if (!Item.has_value()) {
        return std::numeric_limits<std::size_t>::max();
      }
      auto It = Numbers->find(*Item);
      if (It == Numbers->end()) {
        return std::numeric_limits<std::size_t>::max() - 1;
      }
      return It->second;
    };

    const auto LRank = RankOf(LHS);
    const auto RRank = RankOf(RHS);
    if (LRank != RRank) {
      return LRank < RRank;
    }
    if (!LHS.has_value() || !RHS.has_value()) {
      return false;
    }
    return std::less<N>()(*LHS, *RHS);
  }
};

template <typename M, typename N, typename A> class Context {
public:
  using GraphPtr = std::shared_ptr<const DirectedGraph<N>>;
  using WorkItem = std::optional<N>;
  using WorkListType = std::set<WorkItem, WorkItemComparator<N>>;

  inline static std::size_t Count = 0;
  inline static std::size_t TotalNodes = 0;
  inline static std::size_t LiveNodes = 0;

  explicit Context(M Method)
      : Analysed(false), Method(std::move(Method)), Id(++Count),
        OrderNumbers(std::make_shared<std::map<N, std::size_t>>()),
        WorkList(WorkItemComparator<N>{OrderNumbers}) {}

  Context(M Method, GraphPtr Graph, bool Reverse)
      : Analysed(false), Graph(std::move(Graph)), Method(std::move(Method)),
        Id(++Count), OrderNumbers(std::make_shared<std::map<N, std::size_t>>()),
        WorkList(WorkItemComparator<N>{OrderNumbers}) {
    if (this->Graph) {
      const auto Ordered = computePseudoTopologicalOrder(*this->Graph, Reverse);
      std::size_t Number = 1;
      for (const auto &Node : Ordered) {
        (*OrderNumbers)[Node] = Number++;
      }
      TotalNodes += this->Graph->size();
      LiveNodes += this->Graph->size();
    }
  }

  bool operator<(const Context &Other) const { return Id < Other.Id; }

  void freeMemory() {
    if (Freed.load()) {
      return;
    }
    if (Graph) {
      LiveNodes -= Graph->size();
    }
    InValues.clear();
    OutValues.clear();
    EdgeValues.clear();
    Graph.reset();
    WorkList.clear();
    WorkListOfEdges.clear();
    Freed.store(true);
  }

  GraphPtr getControlFlowGraph() const { return Graph; }

  static std::size_t getCount() { return Count; }

  const A &getEntryValue() const { return EntryValue; }
  const A &getExitValue() const { return ExitValue; }
  std::size_t getId() const { return Id; }
  const M &getMethod() const { return Method; }

  std::recursive_mutex &mutex() const { return Mutex; }

  std::optional<A> getEdgeValue(const N &From, const N &To) const {
    auto It = EdgeValues.find(std::make_pair(From, To));
    if (It == EdgeValues.end()) {
      return std::nullopt;
    }
    return It->second;
  }

  void setEdgeValue(const N &From, const N &To, const A &Value) {
    EdgeValues[std::make_pair(From, To)] = Value;
  }

  const A &getValueAfter(const N &Node) const { return OutValues.at(Node); }
  const A &getValueBefore(const N &Node) const { return InValues.at(Node); }

  WorkListType &getWorkList() { return WorkList; }
  const WorkListType &getWorkList() const { return WorkList; }

  std::deque<std::pair<N, N>> &getWorkListOfEdges() { return WorkListOfEdges; }
  const std::deque<std::pair<N, N>> &getWorkListOfEdges() const {
    return WorkListOfEdges;
  }

  bool isAnalysed() const { return Analysed.load(); }
  bool isFreed() const { return Freed.load(); }
  std::size_t getSummaryVersion() const { return SummaryVersion.load(); }

  void markAnalysed() { Analysed.store(true); }
  void unmarkAnalysed() { Analysed.store(false); }
  void publishSummaryVersion() { ++SummaryVersion; }

  void setEntryValue(const A &Value) { EntryValue = Value; }
  void setExitValue(const A &Value) { ExitValue = Value; }
  void setValueAfter(const N &Node, const A &Value) { OutValues[Node] = Value; }
  void setValueBefore(const N &Node, const A &Value) { InValues[Node] = Value; }

private:
  std::atomic<bool> Analysed{false};
  std::atomic<bool> Freed{false};
  std::atomic<std::size_t> SummaryVersion{0};
  mutable std::recursive_mutex Mutex;
  GraphPtr Graph;
  A EntryValue{};
  A ExitValue{};
  M Method;
  std::size_t Id = 0;

  std::map<N, A> OutValues;
  std::map<N, A> InValues;
  std::map<std::pair<N, N>, A> EdgeValues;

  std::shared_ptr<std::map<N, std::size_t>> OrderNumbers;
  WorkListType WorkList;
  std::deque<std::pair<N, N>> WorkListOfEdges;
};

} // namespace vasco
