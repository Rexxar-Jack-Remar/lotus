#pragma once

#include "CFL/InterleavedDyck/LCL/Solver.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// The same checks run through GoogleTest in Lotus and through the standalone
// runner, so the engine can also be validated without LLVM or GoogleTest.
namespace lotus::cfl::interleaved_dyck::lcl::test {

using Word = std::vector<Label>;

inline void check(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

inline std::string printWord(const Word &word) {
  std::string result;
  for (const auto label : word) {
    result += label.str() + " ";
  }
  return result;
}

inline Graph path(const Word &word) {
  Graph graph;
  graph.addVertex(0);
  for (std::size_t i = 0; i < word.size(); ++i) {
    graph.addEdge(static_cast<Vertex>(i), static_cast<Vertex>(i + 1), word[i]);
  }
  return graph;
}

inline bool step(Label label, std::vector<unsigned> &paren,
                 std::vector<unsigned> &bracket, std::size_t bound) {
  switch (label.kind) {
  case LabelKind::OpenParenthesis:
    if (paren.size() == bound) {
      return false;
    }
    paren.push_back(label.id);
    return true;
  case LabelKind::OpenBracket:
    if (bracket.size() == bound) {
      return false;
    }
    bracket.push_back(label.id);
    return true;
  case LabelKind::CloseParenthesis:
    if (paren.empty() || paren.back() != label.id) {
      return false;
    }
    paren.pop_back();
    return true;
  case LabelKind::CloseBracket:
    if (bracket.empty() || bracket.back() != label.id) {
      return false;
    }
    bracket.pop_back();
    return true;
  case LabelKind::Neutral:
    return true;
  }
  return false;
}

inline bool accepts(const Word &word) {
  std::vector<unsigned> paren, bracket;
  for (const auto label : word) {
    if (!step(label, paren, bracket, word.size())) {
      return false;
    }
  }
  return paren.empty() && bracket.empty();
}

// Independent concrete two-stack search. On a DAG, bound >= |V| is exact.
// On a cyclic graph this is ONLY a witness-producing underapproximation.
inline PairSet concreteReachability(const Graph &graph, std::size_t bound) {
  using Configuration =
      std::tuple<Vertex, std::vector<unsigned>, std::vector<unsigned>>;
  PairSet result;
  for (Vertex start : graph.vertices()) {
    std::set<Configuration> seen;
    std::deque<Configuration> queue;
    const Configuration initial{start, {}, {}};
    queue.push_back(initial);
    seen.insert(initial);
    while (!queue.empty()) {
      const auto current = queue.front();
      queue.pop_front();
      if (std::get<1>(current).empty() && std::get<2>(current).empty()) {
        result.insert({start, std::get<0>(current)});
      }
      for (const Edge &edge : graph.edges()) {
        if (edge.source != std::get<0>(current)) {
          continue;
        }
        auto paren = std::get<1>(current);
        auto bracket = std::get<2>(current);
        if (!step(edge.label, paren, bracket, bound)) {
          continue;
        }
        Configuration next{edge.target, std::move(paren), std::move(bracket)};
        if (seen.insert(next).second) {
          queue.push_back(std::move(next));
        }
      }
    }
  }
  return result;
}

inline void subset(const PairSet &left, const PairSet &right,
                   const std::string &context) {
  for (const Pair &pair : left) {
    check(right.count(pair) != 0U,
          context + ": missing " + std::to_string(pair.source) + " -> " +
              std::to_string(pair.target));
  }
}

inline std::array<Label, 8> typedAlphabet() {
  return {Label::openParenthesis(0), Label::closeParenthesis(0),
          Label::openBracket(0), Label::closeBracket(0),
          Label::openParenthesis(1), Label::closeParenthesis(1),
          Label::openBracket(1), Label::closeBracket(1)};
}

inline Graph reordered(const Graph &graph, std::mt19937 &random) {
  auto edges = graph.edges();
  auto vertices = graph.vertices();
  std::shuffle(edges.begin(), edges.end(), random);
  std::shuffle(vertices.begin(), vertices.end(), random);
  Graph result;
  for (Vertex vertex : vertices) {
    result.addVertex(vertex);
  }
  for (const Edge &edge : edges) {
    result.addEdge(edge.source, edge.target, edge.label);
  }
  return result;
}

inline void emptyAndIsolated() {
  const Solver solver;
  check(solver.analyze(Graph{}).upper_bound.empty(), "empty graph");
  Graph graph;
  graph.addVertex(-7);
  graph.addVertex(42);
  const auto result = solver.analyze(graph);
  check(result.upper_bound.size() == 2, "isolated reflexive pairs");
  check(result.mayReach(-7, -7) && result.mayReach(42, 42), "reflexivity");
  check(!result.mayReach(-7, 42) && !result.mayReach(99, 99), "unknown vertices");
  check(result.statistics.summaries == 0, "no phantom trellis summaries");
}

inline void crossingAndNesting() {
  const Solver solver;
  const Word crossing{Label::openParenthesis(1), Label::openBracket(2),
                      Label::closeParenthesis(1), Label::closeBracket(2)};
  const auto result = solver.analyze(path(crossing));
  check(result.mayReach(0, 4), "Figure 4 crossing word");
  check(!result.mayReach(4, 0), "no synthetic reverse arcs");
  const Word nested{Label::openParenthesis(0), Label::openParenthesis(1),
                    Label::openBracket(0), Label::closeParenthesis(1),
                    Label::openBracket(1), Label::closeBracket(1),
                    Label::closeParenthesis(0), Label::closeBracket(0)};
  check(solver.analyze(path(nested)).mayReach(0, 8), "typed nesting/crossing");
  Word concatenated = crossing;
  concatenated.insert(concatenated.end(), nested.begin(), nested.end());
  check(solver.analyze(path(concatenated)).mayReach(0, 12), "concatenation");
}

inline void mismatchesAndUnderflow() {
  const std::vector<Word> invalid{
      {Label::openParenthesis(1), Label::closeParenthesis(2)},
      {Label::openBracket(1), Label::closeBracket(2)},
      {Label::closeParenthesis(0), Label::openParenthesis(0)},
      {Label::closeBracket(0), Label::openBracket(0)},
      {Label::openParenthesis(1), Label::closeBracket(1)},
      {Label::openParenthesis(0), Label::openParenthesis(1),
       Label::closeParenthesis(0), Label::closeParenthesis(1)},
      {Label::openBracket(0), Label::openBracket(1), Label::closeBracket(0),
       Label::closeBracket(1)}};
  for (const auto &word : invalid) {
    check(!Solver{}.analyze(path(word)).mayReach(0, static_cast<Vertex>(word.size())),
          "rejected word: " + printWord(word));
  }
}

inline void grayNodesRejectSpuriousAcceptance() {
  Options baseline;
  baseline.algorithm = Algorithm::Baseline;
  for (const Word &word :
       {Word{Label::openBracket(2), Label::closeParenthesis(1),
             Label::closeBracket(2)},
        Word{Label::openParenthesis(2), Label::closeBracket(1),
             Label::closeParenthesis(2)}}) {
    const Graph graph = path(word);
    check(Solver{}.analyze(graph, baseline).mayReach(0, 3),
          "baseline should retain the white accepting summary");
    Options no_feasibility;
    no_feasibility.enable_feasibility = false;
    check(!Solver{}.analyze(graph, no_feasibility).mayReach(0, 3),
          "gray provenance must reject spurious acceptance even without filters");
    check(!Solver{}.analyze(graph).mayReach(0, 3), "refined gray rejection");
  }
}

inline void neutralClosure() {
  Graph graph;
  graph.addVertex(99);
  graph.addEdge(0, 1, Label::neutral());
  graph.addEdge(1, 2, Label::neutral());
  graph.addEdge(2, 1, Label::neutral());
  graph.addEdge(2, 3, Label::openParenthesis(7));
  graph.addEdge(3, 4, Label::neutral());
  graph.addEdge(4, 5, Label::openBracket(8));
  graph.addEdge(5, 6, Label::neutral());
  graph.addEdge(6, 7, Label::closeParenthesis(7));
  graph.addEdge(7, 8, Label::neutral());
  graph.addEdge(8, 9, Label::closeBracket(8));
  graph.addEdge(9, 10, Label::neutral());
  const auto before = graph.edges();
  const auto result = Solver{}.analyze(graph);
  check(result.mayReach(0, 10), "epsilon prefix, middle, and suffix");
  check(result.mayReach(0, 2) && result.mayReach(2, 1), "epsilon cycle closure");
  check(!result.mayReach(1, 0) && !result.mayReach(10, 0), "directed epsilon");
  check(result.mayReach(99, 99) && !result.mayReach(0, 99), "isolated vertex");
  check(graph.edges() == before, "input must not be modified");
  subset(concreteReachability(graph, 3), result.upper_bound, "neutral witnesses");

  Graph only_neutral;
  only_neutral.addEdge(-1, 0, Label::neutral());
  only_neutral.addEdge(0, 1, Label::neutral());
  const auto pure = Solver{}.analyze(only_neutral);
  check(pure.upper_bound == concreteReachability(only_neutral, 0),
        "neutral-only relation must be exact");
  check(pure.statistics.normalized_edges == 0 && pure.statistics.summaries == 0,
        "neutral is epsilon, not a trellis terminal");
}

inline void sparseIdsAndParallelArcs() {
  const Vertex low = std::numeric_limits<Vertex>::min();
  const Vertex high = std::numeric_limits<Vertex>::max();
  const unsigned id = std::numeric_limits<unsigned>::max();
  Graph graph;
  graph.addEdge(low, -7, Label::openParenthesis(id));
  graph.addEdge(low, -7, Label::openParenthesis(0));
  graph.addEdge(low, -7, Label::closeBracket(id));
  graph.addEdge(-7, 42, Label::openBracket(id));
  graph.addEdge(42, 1234, Label::closeParenthesis(id));
  graph.addEdge(1234, high, Label::closeBracket(id));
  check(!graph.addEdge(low, -7, Label::openParenthesis(id)), "deduplication");
  const auto result = Solver{}.analyze(graph);
  check(result.mayReach(low, high), "full-width vertex and type identifiers");
  check(!result.mayReach(high, low), "sparse identifiers preserve direction");
  subset(concreteReachability(graph, 3), result.upper_bound, "parallel arcs");
}

inline void distinctProjectionWitnesses() {
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(0));
  graph.addEdge(1, 2, Label::openBracket(0));
  graph.addEdge(2, 3, Label::closeParenthesis(0));
  graph.addEdge(0, 4, Label::openBracket(0));
  graph.addEdge(4, 5, Label::openParenthesis(0));
  graph.addEdge(5, 3, Label::closeBracket(0));
  check(!Solver{}.analyze(graph).mayReach(0, 3),
        "do not implement independent projection intersection instead of LCL");
}

inline void checkWord(const Word &word) {
  const bool expected = accepts(word);
  const Graph graph = path(word);
  const Vertex target = static_cast<Vertex>(word.size());
  const auto result = Solver{}.analyze(graph);
  check(result.mayReach(0, target) == expected,
        "linear-path oracle mismatch: " + printWord(word));
  Options no_feasibility;
  no_feasibility.enable_feasibility = false;
  check(Solver{}.analyze(graph, no_feasibility).mayReach(0, target) == expected,
        "unfiltered GWTA oracle mismatch: " + printWord(word));
}

inline void exhaustiveWords(unsigned alphabet_size, unsigned max_length) {
  const auto alphabet = typedAlphabet();
  Word word;
  for (unsigned length = 0; length <= max_length; ++length) {
    word.assign(length, Label::neutral());
    std::size_t count = 1;
    for (unsigned i = 0; i < length; ++i) {
      count *= alphabet_size;
    }
    for (std::size_t code = 0; code < count; ++code) {
      std::size_t value = code;
      for (unsigned i = 0; i < length; ++i) {
        word[i] = alphabet[value % alphabet_size];
        value /= alphabet_size;
      }
      checkWord(word);
    }
  }
}

inline void exhaustiveUnaryWords() { exhaustiveWords(4, 7); }
inline void exhaustiveTypedWords() { exhaustiveWords(8, 5); }

inline void longWords() {
  std::mt19937 random(2017);
  const auto alphabet = typedAlphabet();
  for (unsigned trial = 0; trial < 120; ++trial) {
    Word balanced;
    std::vector<unsigned> paren, bracket;
    for (unsigned i = 0; i < 40; ++i) {
      const auto choice = random() % 4U;
      const auto id = static_cast<unsigned>(random() % 7U);
      Label next;
      if (choice == 0) {
        next = Label::openParenthesis(id);
      } else if (choice == 1) {
        next = Label::openBracket(id);
      } else if (choice == 2 && !paren.empty()) {
        next = Label::closeParenthesis(paren.back());
      } else if (choice == 3 && !bracket.empty()) {
        next = Label::closeBracket(bracket.back());
      } else {
        next = Label::neutral();
      }
      check(step(next, paren, bracket, 100), "balanced-word generator");
      balanced.push_back(next);
    }
    while (!paren.empty() || !bracket.empty()) {
      if (!paren.empty() && (bracket.empty() || random() % 2U == 0)) {
        balanced.push_back(Label::closeParenthesis(paren.back()));
        paren.pop_back();
      } else {
        balanced.push_back(Label::closeBracket(bracket.back()));
        bracket.pop_back();
      }
    }
    checkWord(balanced);
    Word noise;
    for (unsigned i = 0; i < 30; ++i) {
      noise.push_back(alphabet[random() % alphabet.size()]);
    }
    checkWord(noise);
  }
  Word deep;
  for (unsigned i = 0; i < 96; ++i) {
    deep.push_back(Label::openParenthesis(i));
  }
  for (unsigned i = 96; i != 0; --i) {
    deep.push_back(Label::closeParenthesis(i - 1));
  }
  checkWord(deep); // No implicit stack-depth bound in either engine mode.
}

inline void verifyBounds(const Graph &graph, const PairSet &witnesses) {
  Options baseline;
  baseline.algorithm = Algorithm::Baseline;
  Options unfiltered;
  unfiltered.enable_feasibility = false;
  const auto refined = Solver{}.analyze(graph);
  const auto gray_only = Solver{}.analyze(graph, unfiltered);
  const auto white_only = Solver{}.analyze(graph, baseline);
  subset(witnesses, refined.upper_bound, "sound upper bound");
  subset(refined.upper_bound, gray_only.upper_bound, "feasibility refinement");
  subset(gray_only.upper_bound, white_only.upper_bound, "gray refinement");
  for (const auto *result : {&refined, &gray_only, &white_only}) {
    check(result->statistics.worklist_pops <= 2 * result->statistics.summaries,
          "each summary is scheduled at most twice");
  }
}

inline void randomDagSoundness() {
  std::mt19937 random(0xDA612017U);
  const auto alphabet = typedAlphabet();
  for (unsigned trial = 0; trial < 300; ++trial) {
    Graph graph;
    const Vertex n = 3 + static_cast<Vertex>(random() % 6U);
    for (Vertex i = 0; i < n; ++i) {
      graph.addVertex(i);
      for (Vertex j = i + 1; j < n; ++j) {
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
          if (random() % 3U == 0) {
            const auto choice = random() % 9U;
            graph.addEdge(i, j, choice == 8 ? Label::neutral() : alphabet[choice]);
          }
        }
      }
    }
    verifyBounds(graph, concreteReachability(graph, static_cast<std::size_t>(n)));
  }
}

inline void cyclicWitnessSoundness() {
  std::mt19937 random(0xC1C12017U);
  const auto alphabet = typedAlphabet();
  for (unsigned trial = 0; trial < 150; ++trial) {
    Graph graph;
    for (Vertex i = 0; i < 5; ++i) {
      graph.addVertex(i);
    }
    for (unsigned i = 0; i < 11; ++i) {
      const auto choice = random() % 9U;
      graph.addEdge(static_cast<Vertex>(random() % 5U),
                    static_cast<Vertex>(random() % 5U),
                    choice == 8 ? Label::neutral() : alphabet[choice]);
    }
    verifyBounds(graph, concreteReachability(graph, 2));
  }
}

inline void orderIndependence() {
  std::mt19937 random(0x0D3E2017U);
  const auto alphabet = typedAlphabet();
  for (unsigned trial = 0; trial < 150; ++trial) {
    Graph graph;
    for (unsigned i = 0; i < 15; ++i) {
      const auto choice = random() % 9U;
      graph.addEdge(static_cast<Vertex>(random() % 5U),
                    static_cast<Vertex>(random() % 5U),
                    choice == 8 ? Label::neutral() : alphabet[choice]);
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
      Options options;
      options.algorithm = mode == 0 ? Algorithm::Baseline : Algorithm::Refined;
      options.enable_feasibility = mode != 1;
      const auto expected = Solver{}.analyze(graph, options).upper_bound;
      for (unsigned order = 0; order < 3; ++order) {
        check(Solver{}.analyze(reordered(graph, random), options).upper_bound == expected,
              "edge/vertex insertion order changed the fixed point");
      }
    }
  }
}

// A deliberately slow, synchronous reference closure. It does NOT use the
// engine's worklist, summary types, term caches, transition function, or masks.
// q=0/1 are q1/q2; q=2..5 seek P0/P1/B0/B1, q=6..9 defer them;
// z=0 is # and z=1..4 are P0/P1/B0/B1. Inputs use just these two types.
inline PairSet referenceClosure(const Graph &graph, const Options &options) {
  using Key = std::tuple<Vertex, Vertex, int, int>;
  std::map<Key, bool> facts;
  std::map<std::pair<int, int>, std::pair<int, int>> rules;
  for (int z = 0; z <= 4; ++z) {
    rules[{z, 0}] = {0, z};
    rules[{z, 1}] = {z == 0 ? 1 : 0, z};
  }
  for (int type = 0; type < 4; ++type) {
    const int seek = 2 + type;
    const int deferred = 6 + type;
    rules[{0, seek}] = {seek, 0};
    rules[{type + 1, seek}] = {1, 0};
    rules[{0, deferred}] = {deferred, 0};
    rules[{type + 1, deferred}] = {0, 0};
    for (int opposite = 0; opposite < 4; ++opposite) {
      if (type / 2 != opposite / 2) {
        rules[{opposite + 1, seek}] = {deferred, opposite + 1};
        rules[{opposite + 1, deferred}] = {deferred, opposite + 1};
      }
    }
  }
  const bool refined = options.algorithm == Algorithm::Refined;
  const bool filters = refined && options.enable_feasibility;
  std::map<Pair, std::vector<LabelKind>,
           bool (*)(const Pair &, const Pair &)> arcs(
      [](const Pair &a, const Pair &b) {
        return std::tie(a.source, a.target) < std::tie(b.source, b.target);
      });
  for (const Edge &edge : graph.edges()) {
    check(edge.label.kind != LabelKind::Neutral && edge.label.id < 2,
          "reference input alphabet");
    const bool paren = edge.label.kind == LabelKind::OpenParenthesis ||
                       edge.label.kind == LabelKind::CloseParenthesis;
    const bool open = edge.label.kind == LabelKind::OpenParenthesis ||
                      edge.label.kind == LabelKind::OpenBracket;
    const int type = static_cast<int>(edge.label.id) + (paren ? 0 : 2);
    facts[{edge.source, edge.target, open ? 0 : 2 + type, open ? 1 + type : 0}] =
        refined && open;
    arcs[{edge.source, edge.target}].push_back(edge.label.kind);
  }
  const auto out_ok = [filters](int q, LabelKind kind) {
    if (!filters) {
      return true;
    }
    if (kind == LabelKind::OpenParenthesis || kind == LabelKind::OpenBracket) {
      return q == 0;
    }
    const bool seeks_p = q == 2 || q == 3 || q == 6 || q == 7;
    const bool seeks_b = q == 4 || q == 5 || q == 8 || q == 9;
    return kind == LabelKind::CloseParenthesis ? !seeks_b : !seeks_p;
  };
  const auto in_ok = [filters](int z, LabelKind kind) {
    if (!filters) {
      return true;
    }
    if (kind == LabelKind::CloseParenthesis || kind == LabelKind::CloseBracket) {
      return z == 0;
    }
    return kind == LabelKind::OpenParenthesis ? z < 3 : z == 0 || z > 2;
  };
  bool changed = true;
  while (changed) {
    changed = false;
    const auto previous = facts;
    for (const auto &left : previous) {
      for (const auto &right : previous) {
        const Vertex source = std::get<0>(left.first);
        const Vertex target = std::get<1>(right.first);
        const auto rule = rules.find({std::get<3>(left.first), std::get<2>(right.first)});
        if (rule == rules.end()) {
          continue;
        }
        const auto incoming = arcs.find({source, std::get<0>(right.first)});
        const auto outgoing = arcs.find({std::get<1>(left.first), target});
        if (incoming == arcs.end() || outgoing == arcs.end()) {
          continue;
        }
        const int q = rule->second.first;
        const int z = rule->second.second;
        const bool feasible_in = std::any_of(
            incoming->second.begin(), incoming->second.end(),
            [&](LabelKind kind) { return in_ok(z, kind); });
        const bool feasible_out = std::any_of(
            outgoing->second.begin(), outgoing->second.end(),
            [&](LabelKind kind) { return out_ok(q, kind); });
        if (!feasible_in || !feasible_out) {
          continue;
        }
        const bool gray = refined && left.second && (q == 0 || q == 1);
        auto inserted = facts.emplace(Key{source, target, q, z}, gray);
        if (inserted.second) {
          changed = true;
        } else if (gray && !inserted.first->second) {
          inserted.first->second = true;
          changed = true;
        }
      }
    }
  }
  PairSet result;
  for (Vertex v : graph.vertices()) {
    result.insert({v, v});
  }
  for (const auto &fact : facts) {
    if (std::get<2>(fact.first) == 1 && std::get<3>(fact.first) == 0 &&
        (!refined || fact.second)) {
      result.insert({std::get<0>(fact.first), std::get<1>(fact.first)});
    }
  }
  return result;
}

inline void knownOverapproximation() {
  // The only 0->6 words are [0 ]1 and [0 ]0 ]0. Neither is balanced,
  // but endpoint summaries can combine different overlapping witnesses.
  Graph graph;
  graph.addEdge(0, 1, Label::openBracket(0));
  graph.addEdge(1, 4, Label::closeBracket(0));
  graph.addEdge(1, 6, Label::closeBracket(1));
  graph.addEdge(4, 6, Label::closeBracket(0));
  const auto exact = concreteReachability(graph, graph.vertices().size());
  check(exact.count({0, 6}) == 0U, "DAG oracle must reject both concrete paths");
  check(Solver{}.analyze(graph).mayReach(0, 6),
        "a may-reach result must not be presented as a concrete witness");
}

inline void lateGrayPromotion() {
  const std::array<Edge, 3> edges{{
      {0, 1, Label::openBracket(1)},
      {0, 0, Label::closeBracket(1)},
      {1, 0, Label::openParenthesis(0)}}};
  std::array<unsigned, 3> order{{0, 1, 2}};
  std::size_t promotions = 0;
  PairSet expected;
  bool first = true;
  do {
    Graph graph;
    for (unsigned i : order) {
      graph.addEdge(edges[i].source, edges[i].target, edges[i].label);
    }
    const auto result = Solver{}.analyze(graph);
    check(result.upper_bound == referenceClosure(graph, {}),
          "late gray promotions must reach the synchronous fixed point");
    if (first) {
      expected = result.upper_bound;
      first = false;
    }
    check(result.upper_bound == expected, "gray promotion order independence");
    promotions += result.statistics.summary_upgrades;
  } while (std::next_permutation(order.begin(), order.end()));
  check(promotions != 0, "fixture must exercise a late white-to-gray upgrade");
}

inline void referenceFixedPoint() {
  std::mt19937 random(0x51A02017U);
  const auto alphabet = typedAlphabet();
  std::size_t promotions = 0;
  for (unsigned trial = 0; trial < 100; ++trial) {
    Graph graph;
    for (unsigned i = 0; i < 8; ++i) {
      graph.addEdge(static_cast<Vertex>(random() % 3U),
                    static_cast<Vertex>(random() % 3U),
                    alphabet[random() % alphabet.size()]);
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
      Options options;
      options.algorithm = mode == 0 ? Algorithm::Baseline : Algorithm::Refined;
      options.enable_feasibility = mode != 1;
      const auto result = Solver{}.analyze(graph, options);
      check(result.upper_bound == referenceClosure(graph, options),
            "semi-naive saturation differs from synchronous reference closure");
      promotions += result.statistics.summary_upgrades;
    }
  }
  check(promotions != 0, "reference suite must exercise white-to-gray upgrades");
}

inline void resourceLimitsAndValidation() {
  const Graph graph = path({Label::openParenthesis(0), Label::closeParenthesis(0)});
  Options options;
  options.max_summaries = 1;
  bool threw = false;
  try {
    (void)Solver{}.analyze(graph, options);
  } catch (const std::length_error &) {
    threw = true;
  }
  check(threw, "summary limit must throw, not return partial results");
  options.max_summaries = 0;
  options.max_normalized_edges = 1;
  threw = false;
  try {
    (void)Solver{}.analyze(graph, options);
  } catch (const std::length_error &) {
    threw = true;
  }
  check(threw, "edge limit without epsilon");
  Graph epsilon_graph = graph;
  epsilon_graph.addEdge(-1, 0, Label::neutral());
  options.max_normalized_edges = 2;
  threw = false;
  try {
    (void)Solver{}.analyze(epsilon_graph, options);
  } catch (const std::length_error &) {
    threw = true;
  }
  check(threw, "edge limit during epsilon expansion");
  options = {};
  options.algorithm = static_cast<Algorithm>(99);
  threw = false;
  try {
    (void)Solver{}.analyze(graph, options);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  check(threw, "invalid algorithm");
  Graph invalid;
  invalid.addEdge(0, 1, {static_cast<LabelKind>(99), 0});
  threw = false;
  try {
    (void)Solver{}.analyze(invalid);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  check(threw, "invalid label");
  check(Solver{}.analyze(graph).mayReach(0, 2), "fresh call after failure");
}

inline void dotAndReuse() {
  std::istringstream input("digraph G {\n"
                           "0 -> 1 [label=\"op--7\"];\n"
                           "1 -> 2 [label=\"normal\"];\n"
                           "2 -> 3 [label=\"ob--9\"];\n"
                           "3 -> 4 [label=\"cp--7\"];\n"
                           "4 -> 5 [label=\"cb--9\"];\n}\n");
  const Graph graph = Graph::parseDot(input);
  const Solver solver;
  const auto first = solver.analyze(graph);
  check(first.mayReach(0, 5), "shared DOT parser integration");
  check(solver.analyze(Graph{}).upper_bound.empty(), "no cross-call state");
  check(solver.analyze(graph).upper_bound == first.upper_bound, "repeated solve");
}

struct TestCase {
  const char *name;
  void (*run)();
};

inline const std::vector<TestCase> &cases() {
  static const std::vector<TestCase> tests{
      {"emptyAndIsolated", emptyAndIsolated},
      {"crossingAndNesting", crossingAndNesting},
      {"mismatchesAndUnderflow", mismatchesAndUnderflow},
      {"grayNodesRejectSpuriousAcceptance", grayNodesRejectSpuriousAcceptance},
      {"neutralClosure", neutralClosure},
      {"sparseIdsAndParallelArcs", sparseIdsAndParallelArcs},
      {"distinctProjectionWitnesses", distinctProjectionWitnesses},
      {"exhaustiveUnaryWords", exhaustiveUnaryWords},
      {"exhaustiveTypedWords", exhaustiveTypedWords},
      {"longWords", longWords},
      {"randomDagSoundness", randomDagSoundness},
      {"cyclicWitnessSoundness", cyclicWitnessSoundness},
      {"orderIndependence", orderIndependence},
      {"knownOverapproximation", knownOverapproximation},
      {"lateGrayPromotion", lateGrayPromotion},
      {"referenceFixedPoint", referenceFixedPoint},
      {"resourceLimitsAndValidation", resourceLimitsAndValidation},
      {"dotAndReuse", dotAndReuse}};
  return tests;
}

} // namespace lotus::cfl::interleaved_dyck::lcl::test
