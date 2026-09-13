#include "CFL/InterleavedDyck/StagedBounds/Algorithms.h"

#include <cstddef>
#include <unordered_map>

namespace lotus::cfl::interleaved_dyck::staged_bounds::detail {

Graph automatonProduct(const Graph &graph, BenchmarkKind benchmark,
                       std::size_t &state_count, std::size_t &accept_state) {
  Graph product;
  if (benchmark == BenchmarkKind::ValueFlow) {
    state_count = 6;
    accept_state = 2;
    for (const Edge &edge : graph.edges()) {
      const auto add = [&](std::size_t from, std::size_t to, Label label) {
        product.addEdge(productVertex(edge.source, state_count, from),
                        productVertex(edge.target, state_count, to), label);
      };
      if (edge.label.kind == LabelKind::OpenBracket) {
        add(0, 3, Label::neutral());
        add(1, 3, Label::neutral());
        add(2, 3, Label::neutral());
        add(3, 4, Label::neutral());
        add(4, 4, Label::neutral());
        add(5, 4, Label::neutral());
      } else if (edge.label.kind == LabelKind::CloseBracket) {
        add(3, 2, Label::neutral());
        add(4, 5, Label::neutral());
        add(5, 5, Label::neutral());
      } else {
        add(1, 1, edge.label);
        add(2, 1, edge.label);
        add(3, 3, edge.label);
        add(4, 4, edge.label);
        add(5, 4, edge.label);
      }
    }
    return product;
  }

  const auto brackets = labelIds(graph, Alphabet::Bracket);
  state_count = brackets.size() + 2;
  accept_state = 0;
  std::unordered_map<unsigned, std::size_t> bracket_state;
  for (std::size_t index = 0; index < brackets.size(); ++index) {
    bracket_state[brackets[index]] = index + 1;
  }
  for (const Edge &edge : graph.edges()) {
    const auto add = [&](std::size_t from, std::size_t to, Label label) {
      product.addEdge(productVertex(edge.source, state_count, from),
                      productVertex(edge.target, state_count, to), label);
    };
    if (edge.label.kind == LabelKind::OpenBracket) {
      for (std::size_t state = 1; state < state_count; ++state) {
        add(state, state_count - 1, Label::neutral());
      }
      add(0, bracket_state.at(edge.label.id), Label::neutral());
    } else if (edge.label.kind == LabelKind::CloseBracket) {
      add(bracket_state.at(edge.label.id), 0, Label::neutral());
      add(state_count - 1, state_count - 1, Label::neutral());
    } else {
      for (std::size_t state = 0; state < state_count; ++state) {
        add(state, state, edge.label);
      }
    }
  }
  return product;
}

PairSet regularization(const Graph &graph, BenchmarkKind benchmark) {
  std::size_t states = 0;
  std::size_t accept_state = 0;
  const Graph product =
      automatonProduct(graph, benchmark, states, accept_state);
  const PairSet product_pairs =
      runProjected(product, Alphabet::Parenthesis, GrammarStrength::Classic, 2)
          .pairs;
  PairSet result;
  for (const Pair &pair : product_pairs) {
    const auto source_remainder = pair.source % static_cast<Vertex>(states);
    auto target_remainder = pair.target % static_cast<Vertex>(states);
    if (source_remainder < 0) {
      continue;
    }
    if (target_remainder < 0) {
      target_remainder += static_cast<Vertex>(states);
    }
    if (source_remainder == 0 &&
        (target_remainder == static_cast<Vertex>(accept_state) ||
         target_remainder == static_cast<Vertex>(states - 1))) {
      const Pair mapped{pair.source / static_cast<Vertex>(states),
                        pair.target / static_cast<Vertex>(states)};
      if (mapped.source != mapped.target) {
        result.insert(mapped);
      }
    }
  }
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::staged_bounds::detail
