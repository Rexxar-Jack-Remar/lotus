#pragma once

#include "Dataflow/Datalog/Atom.h"
#include "Dataflow/Datalog/Expr.h"
#include "Dataflow/Datalog/SemanticIR.h"

#include <any>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace lotus::datalog {

template <typename Input, typename Output> class AggregatorSpec {
public:
  AggregatorSpec(
      Expr<Input> projection, std::string name,
      std::function<std::vector<Output>(const std::vector<Input> &)> evaluator)
      : projection_(std::move(projection)), name_(std::move(name)) {
    evaluator_ = [evaluator = std::move(evaluator)](
                     const std::vector<std::any> &values) {
      std::vector<Input> typed_values;
      typed_values.reserve(values.size());
      for (const std::any &value : values)
        typed_values.push_back(std::any_cast<Input>(value));
      std::vector<Output> typed_results = evaluator(typed_values);
      std::vector<std::any> results;
      results.reserve(typed_results.size());
      for (Output &result : typed_results)
        results.emplace_back(std::move(result));
      return results;
    };
  }

  template <typename MakeState, typename Add, typename Merge, typename Finish>
  AggregatorSpec(Expr<Input> projection, std::string name, MakeState make_state,
                 Add add, Merge merge, Finish finish)
      : projection_(std::move(projection)), name_(std::move(name)) {
    using State = std::invoke_result_t<MakeState>;
    ReducerIR reducer;
    reducer.make_state = [make_state] { return std::any(make_state()); };
    reducer.add = [add](std::any &state, const std::any &value) {
      add(std::any_cast<State &>(state), std::any_cast<const Input &>(value));
    };
    reducer.merge = [merge](std::any &state, const std::any &other) {
      merge(std::any_cast<State &>(state), std::any_cast<const State &>(other));
    };
    reducer.finish = [finish](std::any &state) {
      std::vector<Output> typed_results = finish(std::any_cast<State &>(state));
      std::vector<std::any> results;
      results.reserve(typed_results.size());
      for (Output &result : typed_results)
        results.emplace_back(std::move(result));
      return results;
    };
    reducer_ = std::move(reducer);
    evaluator_ = [reducer = *reducer_](const std::vector<std::any> &values) {
      std::any state = reducer.make_state();
      for (const std::any &value : values)
        reducer.add(state, value);
      return reducer.finish(state);
    };
  }

  Context *context() const { return projection_.context(); }

private:
  Expr<Input> projection_;
  std::string name_;
  std::function<std::vector<std::any>(const std::vector<std::any> &)>
      evaluator_;
  std::optional<ReducerIR> reducer_;

  template <typename In, typename Out>
  friend AggregateClause
  aggregate(const Var<Out> &, const AggregatorSpec<In, Out> &, const Atom &);
};

template <typename Output, typename Input, typename Function>
AggregatorSpec<Input, Output> make_aggregator(const Expr<Input> &projection,
                                              std::string name,
                                              Function evaluator) {
  return AggregatorSpec<Input, Output>(
      projection, std::move(name),
      std::function<std::vector<Output>(const std::vector<Input> &)>(
          std::move(evaluator)));
}

template <typename T> AggregatorSpec<T, T> sum(const Expr<T> &projection) {
  return AggregatorSpec<T, T>(
      projection, "sum", [] { return T{}; },
      [](T &state, const T &value) { state += value; },
      [](T &state, const T &other) { state += other; },
      [](T &state) { return std::vector<T>{state}; });
}

inline AggregatorSpec<int, std::size_t> count() {
  return AggregatorSpec<int, std::size_t>(
      Expr<int>::constant(0), "count", [] { return std::size_t{0}; },
      [](std::size_t &state, const int &) { ++state; },
      [](std::size_t &state, const std::size_t &other) { state += other; },
      [](std::size_t &state) { return std::vector<std::size_t>{state}; });
}

template <typename T> AggregatorSpec<T, T> minimum(const Expr<T> &projection) {
  struct State {
    std::optional<T> value;
  };
  return AggregatorSpec<T, T>(
      projection, "minimum", [] { return State{}; },
      [](State &state, const T &value) {
        if (!state.value || value < *state.value)
          state.value = value;
      },
      [](State &state, const State &other) {
        if (other.value &&
            (!state.value || std::less<T>{}(*other.value, *state.value)))
          state.value = other.value;
      },
      [](State &state) {
        return state.value ? std::vector<T>{*state.value} : std::vector<T>{};
      });
}

template <typename T> AggregatorSpec<T, T> maximum(const Expr<T> &projection) {
  struct State {
    std::optional<T> value;
  };
  return AggregatorSpec<T, T>(
      projection, "maximum", [] { return State{}; },
      [](State &state, const T &value) {
        if (!state.value || *state.value < value)
          state.value = value;
      },
      [](State &state, const State &other) {
        if (other.value &&
            (!state.value || std::less<T>{}(*state.value, *other.value)))
          state.value = other.value;
      },
      [](State &state) {
        return state.value ? std::vector<T>{*state.value} : std::vector<T>{};
      });
}

template <typename T>
AggregatorSpec<T, double> mean(const Expr<T> &projection) {
  struct State {
    long double sum = 0;
    std::size_t count = 0;
  };
  return AggregatorSpec<T, double>(
      projection, "mean", [] { return State{}; },
      [](State &state, const T &value) {
        state.sum += static_cast<long double>(value);
        ++state.count;
      },
      [](State &state, const State &other) {
        state.sum += other.sum;
        state.count += other.count;
      },
      [](State &state) {
        if (state.count == 0)
          return std::vector<double>{};
        return std::vector<double>{
            static_cast<double>(state.sum / state.count)};
      });
}

class AggregateClause {
public:
  Context *context() const { return context_; }
  const AggregateIR &ir() const { return ir_; }

private:
  AggregateClause(Context *context, AggregateIR ir)
      : context_(context), ir_(std::move(ir)) {}

  Context *context_ = nullptr;
  AggregateIR ir_;

  template <typename Input, typename Output>
  friend AggregateClause aggregate(const Var<Output> &,
                                   const AggregatorSpec<Input, Output> &,
                                   const Atom &);
  friend class Body;
  friend class Program;
};

template <typename Input, typename Output>
AggregateClause aggregate(const Var<Output> &output,
                          const AggregatorSpec<Input, Output> &aggregator,
                          const Atom &source) {
  Context *context =
      detail::mergeContexts(output.context(), aggregator.context());
  context = detail::mergeContexts(context, source.context());
  AggregateIR ir;
  ir.output_var = output.id();
  ir.output_type = typeid(Output);
  ir.source = source.ir();
  ir.projection = aggregator.projection_.lower();
  ir.name = aggregator.name_;
  ir.evaluate_range = aggregator.evaluator_;
  ir.reducer = aggregator.reducer_;
  return AggregateClause(context, std::move(ir));
}

} // namespace lotus::datalog
