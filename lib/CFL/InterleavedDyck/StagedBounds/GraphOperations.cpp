#include "CFL/InterleavedDyck/StagedBounds/Algorithms.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lotus::cfl::interleaved_dyck::staged_bounds::detail {

bool isParenthesis(LabelKind kind) {
  return kind == LabelKind::OpenParenthesis ||
         kind == LabelKind::CloseParenthesis;
}

bool isBracket(LabelKind kind) {
  return kind == LabelKind::OpenBracket || kind == LabelKind::CloseBracket;
}

bool isOpen(LabelKind kind) {
  return kind == LabelKind::OpenParenthesis || kind == LabelKind::OpenBracket;
}

bool belongsTo(LabelKind kind, Alphabet alphabet) {
  return alphabet == Alphabet::Parenthesis ? isParenthesis(kind)
                                           : isBracket(kind);
}

Label openLabel(Alphabet alphabet, unsigned id) {
  return alphabet == Alphabet::Parenthesis ? Label::openParenthesis(id)
                                           : Label::openBracket(id);
}

Label closeLabel(Alphabet alphabet, unsigned id) {
  return alphabet == Alphabet::Parenthesis ? Label::closeParenthesis(id)
                                           : Label::closeBracket(id);
}

PairSet intersect(const PairSet &left, const PairSet &right) {
  const PairSet *small = &left;
  const PairSet *large = &right;
  if (small->size() > large->size()) {
    std::swap(small, large);
  }
  PairSet result;
  for (const Pair &pair : *small) {
    if (large->count(pair) != 0U && pair.source != pair.target) {
      result.insert(pair);
    }
  }
  return result;
}

std::vector<unsigned> labelIds(const Graph &graph, Alphabet alphabet) {
  std::unordered_set<unsigned> ids;
  for (const Edge &edge : graph.edges()) {
    if (belongsTo(edge.label.kind, alphabet)) {
      ids.insert(edge.label.id);
    }
  }
  std::vector<unsigned> result(ids.begin(), ids.end());
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<unsigned> matchedLabelIds(const Graph &graph, Alphabet alphabet) {
  std::unordered_set<unsigned> opens;
  std::unordered_set<unsigned> closes;
  for (const Edge &edge : graph.edges()) {
    if (!belongsTo(edge.label.kind, alphabet)) {
      continue;
    }
    (isOpen(edge.label.kind) ? opens : closes).insert(edge.label.id);
  }
  std::vector<unsigned> result;
  for (unsigned id : opens) {
    if (closes.count(id) != 0U) {
      result.push_back(id);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

Graph retainMatchedLabels(const Graph &graph) {
  const auto parenthesis = matchedLabelIds(graph, Alphabet::Parenthesis);
  const auto bracket = matchedLabelIds(graph, Alphabet::Bracket);
  const std::unordered_set<unsigned> parenthesis_set(parenthesis.begin(),
                                                     parenthesis.end());
  const std::unordered_set<unsigned> bracket_set(bracket.begin(),
                                                 bracket.end());
  Graph result;
  for (const Edge &edge : graph.edges()) {
    if (edge.label.kind == LabelKind::Neutral ||
        (isParenthesis(edge.label.kind) &&
         parenthesis_set.count(edge.label.id) != 0U) ||
        (isBracket(edge.label.kind) &&
         bracket_set.count(edge.label.id) != 0U)) {
      result.addEdge(edge.source, edge.target, edge.label);
    }
  }
  return result;
}

using Adjacency =
    std::unordered_map<Vertex, std::vector<std::pair<Vertex, std::size_t>>>;

Adjacency adjacency(const Graph &graph, bool reverse = false) {
  Adjacency result;
  for (std::size_t index = 0; index < graph.edges().size(); ++index) {
    const Edge &edge = graph.edges()[index];
    const Vertex from = reverse ? edge.target : edge.source;
    const Vertex to = reverse ? edge.source : edge.target;
    result[from].push_back({to, index});
  }
  return result;
}

std::unordered_set<Vertex> reachable(const Adjacency &edges,
                                     const std::vector<Vertex> &starts) {
  std::unordered_set<Vertex> seen;
  std::deque<Vertex> worklist;
  for (Vertex start : starts) {
    if (seen.insert(start).second) {
      worklist.push_back(start);
    }
  }
  while (!worklist.empty()) {
    const Vertex current = worklist.front();
    worklist.pop_front();
    const auto found = edges.find(current);
    if (found == edges.end()) {
      continue;
    }
    for (const auto &[next, unused] : found->second) {
      (void)unused;
      if (seen.insert(next).second) {
        worklist.push_back(next);
      }
    }
  }
  return seen;
}

Graph removeNotOnCandidatePaths(const Graph &graph, const PairSet &pairs) {
  if (pairs.empty() || graph.empty()) {
    return {};
  }

  std::unordered_map<Vertex, std::vector<Vertex>> targets_by_source;
  for (const Pair &pair : pairs) {
    targets_by_source[pair.source].push_back(pair.target);
  }

  const Adjacency forward_edges = adjacency(graph);
  const Adjacency reverse_edges = adjacency(graph, true);
  std::vector<bool> keep(graph.edges().size(), false);
  for (const auto &[source, targets] : targets_by_source) {
    const auto from_source = reachable(forward_edges, {source});
    const auto to_target = reachable(reverse_edges, targets);
    for (std::size_t index = 0; index < graph.edges().size(); ++index) {
      const Edge &edge = graph.edges()[index];
      if (from_source.count(edge.source) != 0U &&
          to_target.count(edge.target) != 0U) {
        keep[index] = true;
      }
    }
  }

  Graph result;
  for (std::size_t index = 0; index < graph.edges().size(); ++index) {
    if (keep[index]) {
      const Edge &edge = graph.edges()[index];
      result.addEdge(edge.source, edge.target, edge.label);
    }
  }
  return result;
}

Graph removeValueFlowUnreachable(const Graph &graph) {
  std::vector<Vertex> sources;
  std::vector<Vertex> sinks;
  for (const Edge &edge : graph.edges()) {
    if (edge.label.kind == LabelKind::OpenBracket) {
      sources.push_back(edge.source);
    } else if (edge.label.kind == LabelKind::CloseBracket) {
      sinks.push_back(edge.target);
    }
  }
  if (sources.empty() || sinks.empty()) {
    return {};
  }

  const auto after_source = reachable(adjacency(graph), sources);
  const auto before_sink = reachable(adjacency(graph, true), sinks);
  Graph result;
  for (const Edge &edge : graph.edges()) {
    if (after_source.count(edge.source) != 0U &&
        after_source.count(edge.target) != 0U &&
        before_sink.count(edge.source) != 0U &&
        before_sink.count(edge.target) != 0U) {
      result.addEdge(edge.source, edge.target, edge.label);
    }
  }
  return result;
}

PairSet filterBracketPaths(const Graph &graph, const PairSet &pairs) {
  std::unordered_map<Vertex, std::vector<Vertex>> opens;
  std::unordered_map<Vertex, std::vector<Vertex>> closes;
  for (const Edge &edge : graph.edges()) {
    if (edge.label == Label::openBracket(0)) {
      opens[edge.source].push_back(edge.target);
    } else if (edge.label == Label::closeBracket(0)) {
      closes[edge.target].push_back(edge.source);
    }
  }

  const Adjacency forward = adjacency(graph);
  std::unordered_map<Vertex, std::unordered_set<Vertex>> reach_cache;
  PairSet result;
  for (const Pair &pair : pairs) {
    const auto open_it = opens.find(pair.source);
    const auto close_it = closes.find(pair.target);
    if (open_it == opens.end() || close_it == closes.end()) {
      continue;
    }
    bool accepted = false;
    for (Vertex open_target : open_it->second) {
      auto [cache_it, inserted] = reach_cache.try_emplace(open_target);
      if (inserted) {
        cache_it->second = reachable(forward, {open_target});
      }
      for (Vertex close_source : close_it->second) {
        if (cache_it->second.count(close_source) != 0U) {
          accepted = true;
          break;
        }
      }
      if (accepted) {
        break;
      }
    }
    if (accepted && pair.source != pair.target) {
      result.insert(pair);
    }
  }
  return result;
}
Vertex productVertex(Vertex vertex, std::size_t states, std::size_t state) {
  const __int128 value = static_cast<__int128>(vertex) * states + state;
  if (value < std::numeric_limits<Vertex>::min() ||
      value > std::numeric_limits<Vertex>::max()) {
    throw std::overflow_error("product-automaton vertex id overflow");
  }
  return static_cast<Vertex>(value);
}
Graph valueFlowTransform(const Graph &graph) {
  Graph transformed;
  for (const Edge &edge : graph.edges()) {
    transformed.addEdge(productVertex(edge.source, 3, 1),
                        productVertex(edge.target, 3, 1), edge.label);
    if (edge.label.kind == LabelKind::OpenBracket) {
      transformed.addEdge(productVertex(edge.source, 3, 0),
                          productVertex(edge.target, 3, 1), edge.label);
    } else if (edge.label.kind == LabelKind::CloseBracket) {
      transformed.addEdge(productVertex(edge.source, 3, 1),
                          productVertex(edge.target, 3, 2), edge.label);
    }
  }
  return transformed;
}

PairSet filterValueFlowPairs(const PairSet &pairs) {
  PairSet result;
  for (const Pair &pair : pairs) {
    Vertex source_mod = pair.source % 3;
    Vertex target_mod = pair.target % 3;
    if (source_mod < 0) {
      source_mod += 3;
    }
    if (target_mod < 0) {
      target_mod += 3;
    }
    if (source_mod == 0 && target_mod == 2) {
      const Pair mapped{pair.source / 3, pair.target / 3};
      if (mapped.source != mapped.target) {
        result.insert(mapped);
      }
    }
  }
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::staged_bounds::detail
