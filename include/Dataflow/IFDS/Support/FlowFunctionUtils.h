/*
 * Reusable IFDS flow-function utilities.
 *
 * The helpers return small callable objects that map one source fact to a set of
 * target facts. They are intended for client analyses and tests; solver APIs do
 * not depend on this header.
 */

#pragma once

#include <functional>
#include <initializer_list>
#include <set>
#include <utility>

namespace ifds {
namespace flow {

template <typename Fact> using FactSet = std::set<Fact>;

template <typename Fact> auto identity() {
  return [](const Fact &fact) { return FactSet<Fact>{fact}; };
}

template <typename Fact> auto kill_all() {
  return [](const Fact &) { return FactSet<Fact>{}; };
}

template <typename Fact, typename Predicate> auto kill_if(Predicate predicate) {
  return [predicate = std::move(predicate)](const Fact &fact) {
    if (predicate(fact)) {
      return FactSet<Fact>{};
    }
    return FactSet<Fact>{fact};
  };
}

template <typename Fact> auto generate(std::initializer_list<Fact> generated) {
  FactSet<Fact> generated_facts(generated.begin(), generated.end());
  return [generated_facts = std::move(generated_facts)](const Fact &fact) {
    FactSet<Fact> result = generated_facts;
    result.insert(fact);
    return result;
  };
}

template <typename Fact, typename Generator> auto generate_if(Generator gen) {
  return [gen = std::move(gen)](const Fact &fact) {
    FactSet<Fact> result{fact};
    auto generated = gen(fact);
    result.insert(generated.begin(), generated.end());
    return result;
  };
}

template <typename Fact, typename Predicate>
auto gen_and_kill(std::initializer_list<Fact> generated, Predicate kill) {
  FactSet<Fact> generated_facts(generated.begin(), generated.end());
  return [generated_facts = std::move(generated_facts),
          kill = std::move(kill)](const Fact &fact) {
    FactSet<Fact> result = generated_facts;
    if (!kill(fact)) {
      result.insert(fact);
    }
    return result;
  };
}

template <typename Fact> auto transfer(const Fact &source, const Fact &target) {
  return [source, target](const Fact &fact) {
    if (fact == source) {
      return FactSet<Fact>{target};
    }
    return FactSet<Fact>{fact};
  };
}

template <typename Fact, typename Predicate, typename Mapper>
auto transfer_if(Predicate predicate, Mapper mapper) {
  return [predicate = std::move(predicate), mapper = std::move(mapper)](
             const Fact &fact) {
    if (predicate(fact)) {
      return FactSet<Fact>{mapper(fact)};
    }
    return FactSet<Fact>{fact};
  };
}

template <typename Fact, typename... FlowFunctions>
auto union_flows(FlowFunctions... functions) {
  return [=](const Fact &fact) {
    FactSet<Fact> result;
    (void)std::initializer_list<int>{
        ( [&] {
            FactSet<Fact> partial = functions(fact);
            result.insert(partial.begin(), partial.end());
          }(),
          0)...};
    return result;
  };
}

} // namespace flow
} // namespace ifds
