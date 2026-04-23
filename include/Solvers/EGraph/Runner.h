#pragma once

#include "Solvers/EGraph/Extract.h"
#include "Solvers/EGraph/Rewrite.h"

namespace lotus::egraph {

enum class StopReasonKind {
  None,
  Saturated,
  IterationLimit,
  NodeLimit,
  TimeLimit,
  Other,
};

struct StopReason {
  StopReasonKind kind = StopReasonKind::None;
  std::string detail;
};

template <typename IterData> struct Iteration {
  size_t index = 0;
  size_t eclasses = 0;
  size_t enodes = 0;
  size_t applied = 0;
  IterData data{};
};

template <typename L, typename A = NoAnalysis<L>, typename IterData = std::monostate>
class Runner {
public:
  explicit Runner(A analysis = A()) : egraph(std::move(analysis)) {}

  Runner &withExpr(const RecExpr<L> &expr) {
    roots.push_back(egraph.addExpr(expr));
    egraph.rebuild();
    return *this;
  }

  Runner &withIterLimit(size_t limit) {
    iter_limit_ = limit;
    return *this;
  }

  Runner &withNodeLimit(size_t limit) {
    node_limit_ = limit;
    return *this;
  }

  Runner &withTimeLimit(Duration limit) {
    time_limit_ = limit;
    return *this;
  }

  Runner &withHook(std::function<bool(Runner &)> hook) {
    hooks.push_back(std::move(hook));
    return *this;
  }

  Runner &run(const std::vector<Rewrite<L, A>> &rules) {
    Instant start = now();
    for (size_t iteration_index = 0; iteration_index < iter_limit_; ++iteration_index) {
      if (egraph.totalSize() > node_limit_) {
        stop_reason = {StopReasonKind::NodeLimit, std::to_string(egraph.totalSize())};
        return *this;
      }
      if (now() - start > time_limit_) {
        stop_reason = {StopReasonKind::TimeLimit, "time limit exceeded"};
        return *this;
      }

      size_t before_size = egraph.totalSize();
      size_t before_classes = egraph.numberOfClasses();
      size_t applied = 0;

      for (const auto &rule : rules) {
        auto matches = rule.search(egraph);
        auto ids = rule.apply(egraph, matches);
        applied += ids.size();
        egraph.rebuild();
      }

      iterations.push_back(Iteration<IterData>{
          iteration_index,
          egraph.numberOfClasses(),
          egraph.totalSize(),
          applied,
          IterData{},
      });

      bool keep_going = true;
      for (auto &hook : hooks) {
        if (!hook(*this)) {
          keep_going = false;
        }
      }
      if (!keep_going) {
        stop_reason = {StopReasonKind::Other, "stopped by hook"};
        return *this;
      }

      if (egraph.totalSize() == before_size &&
          egraph.numberOfClasses() == before_classes) {
        stop_reason = {StopReasonKind::Saturated, {}};
        return *this;
      }
    }

    stop_reason = {StopReasonKind::IterationLimit, std::to_string(iter_limit_)};
    return *this;
  }

  EGraph<L, A> egraph;
  std::vector<Iteration<IterData>> iterations;
  std::vector<Id> roots;
  StopReason stop_reason;
  std::vector<std::function<bool(Runner &)>> hooks;

private:
  size_t iter_limit_ = 30;
  size_t node_limit_ = 10'000;
  Duration time_limit_ = std::chrono::seconds(5);
};

} // namespace lotus::egraph
