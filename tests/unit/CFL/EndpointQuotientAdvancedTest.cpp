#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotient.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <set>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::classical {
namespace {

using endpoint::Id;
using endpoint::Options;
using endpoint::PartitionMode;
using endpoint::Problem;
using endpoint::Rule;
using endpoint::Solver;
using Fact = std::tuple<Id, Id, Id>;

class CubicClosure {
public:
  explicit CubicClosure(const Problem &problem)
      : nodes_(problem.nodes), symbols_(problem.symbols),
        facts_(symbols_ * nodes_ * nodes_, false) {
    for (const auto &edge : problem.edges)
      insert(edge.symbol, edge.source, edge.target);

    bool changed = true;
    while (changed) {
      changed = false;
      for (const Rule &rule : problem.rules) {
        for (Id source = 0; source < nodes_; ++source) {
          if (rule.kind == Rule::Kind::Epsilon) {
            changed = insert(rule.lhs, source, source) || changed;
            continue;
          }
          for (Id target = 0; target < nodes_; ++target) {
            bool derives = false;
            if (rule.kind == Rule::Kind::Unary) {
              derives = contains(rule.left, source, target);
            } else {
              for (Id middle = 0; middle < nodes_ && !derives; ++middle)
                derives = contains(rule.left, source, middle) &&
                          contains(rule.right, middle, target);
            }
            if (derives)
              changed = insert(rule.lhs, source, target) || changed;
          }
        }
      }
    }
  }

  bool contains(Id symbol, Id source, Id target) const {
    return facts_[index(symbol, source, target)];
  }

  std::size_t size() const {
    return static_cast<std::size_t>(
        std::count(facts_.begin(), facts_.end(), true));
  }

  std::set<Fact> facts() const {
    std::set<Fact> result;
    for (Id symbol = 0; symbol < symbols_; ++symbol)
      for (Id source = 0; source < nodes_; ++source)
        for (Id target = 0; target < nodes_; ++target)
          if (contains(symbol, source, target))
            result.emplace(symbol, source, target);
    return result;
  }

private:
  std::size_t index(Id symbol, Id source, Id target) const {
    return (symbol * nodes_ + source) * nodes_ + target;
  }

  bool insert(Id symbol, Id source, Id target) {
    const std::size_t position = index(symbol, source, target);
    if (facts_[position])
      return false;
    facts_[position] = true;
    return true;
  }

  Id nodes_;
  Id symbols_;
  std::vector<bool> facts_;
};

void expectMatchesOracle(const Solver &solver, const Problem &problem) {
  const CubicClosure expected(problem);
  ASSERT_EQ(solver.nodeCount(), problem.nodes);
  ASSERT_EQ(solver.statistics().logical_facts, expected.size());

  std::set<Fact> visited;
  for (Id symbol = 0; symbol < problem.symbols; ++symbol) {
    std::set<std::pair<Id, Id>> symbol_facts;
    ASSERT_TRUE(solver.visitFacts(symbol, [&](Id source, Id target) {
      EXPECT_TRUE(symbol_facts.emplace(source, target).second);
      EXPECT_TRUE(visited.emplace(symbol, source, target).second);
      return true;
    }));

    for (Id source = 0; source < problem.nodes; ++source) {
      std::set<Id> successors;
      ASSERT_TRUE(solver.visitSuccessors(symbol, source, [&](Id target) {
        EXPECT_TRUE(successors.insert(target).second);
        return true;
      }));
      for (Id target = 0; target < problem.nodes; ++target) {
        const bool reachable = expected.contains(symbol, source, target);
        EXPECT_EQ(solver.contains(symbol, source, target), reachable)
            << "symbol=" << symbol << " source=" << source
            << " target=" << target;
        EXPECT_EQ(successors.count(target), reachable ? 1u : 0u);
      }
    }

    for (Id target = 0; target < problem.nodes; ++target) {
      std::set<Id> predecessors;
      ASSERT_TRUE(solver.visitPredecessors(symbol, target, [&](Id source) {
        EXPECT_TRUE(predecessors.insert(source).second);
        return true;
      }));
      for (Id source = 0; source < problem.nodes; ++source)
        EXPECT_EQ(predecessors.count(source),
                  expected.contains(symbol, source, target) ? 1u : 0u);
    }
  }
  EXPECT_EQ(visited, expected.facts());
}

Options options(PartitionMode partitions, bool factorized) {
  Options result;
  result.partitions = partitions;
  result.factorized = factorized;
  return result;
}

TEST(EndpointQuotientAdvancedTest,
     ExhaustiveSmallInputSubsetsMatchCubicClosure) {
  // Every subset of these differently-labelled cycle edges exercises a
  // different endpoint signature. The mutually recursive rules also force
  // rule-local factor views to communicate through unary and binary plans.
  const std::array<endpoint::Edge, 6> candidates{{
      {0, 0, 1}, {1, 0, 2}, {2, 0, 0},
      {0, 1, 2}, {2, 1, 1}, {1, 1, 0},
  }};
  const std::vector<Rule> rules{
      Rule::unary(2, 0), Rule::unary(3, 1), Rule::binary(2, 2, 3),
      Rule::binary(3, 2, 3), Rule::binary(4, 2, 2), Rule::epsilon(3),
  };

  for (std::uint32_t mask = 0; mask < (1u << candidates.size()); ++mask) {
    Problem problem{3, 5, {}, rules};
    for (std::size_t edge = 0; edge < candidates.size(); ++edge)
      if (mask & (1u << edge))
        problem.edges.push_back(candidates[edge]);

    for (bool factorized : {false, true}) {
      SCOPED_TRACE(mask);
      SCOPED_TRACE(factorized);
      Solver solver(problem, options(PartitionMode::Grammar, factorized));
      solver.solve();
      expectMatchesOracle(solver, problem);
    }
  }
}

TEST(EndpointQuotientAdvancedTest,
     RandomizedMixedRecursionMatchesOracleWithBothSaturations) {
  std::mt19937 random(0x45515f41);
  for (unsigned trial = 0; trial < 96; ++trial) {
    Problem problem;
    problem.nodes = 1 + random() % 7;
    problem.symbols = 2 + random() % 5;

    const unsigned edge_count = 1 + random() % 20;
    for (unsigned edge = 0; edge < edge_count; ++edge)
      problem.edges.push_back({random() % problem.nodes,
                               random() % problem.symbols,
                               random() % problem.nodes});

    // Always include self-recursion; the remaining rules mix nullable,
    // mutually recursive, and differently partitioned symbols.
    const Id recursive = random() % problem.symbols;
    problem.rules.push_back(Rule::binary(recursive, recursive, recursive));
    const unsigned rule_count = 4 + random() % 12;
    for (unsigned rule = 0; rule < rule_count; ++rule) {
      const Id lhs = random() % problem.symbols;
      const Id left = random() % problem.symbols;
      const Id right = random() % problem.symbols;
      switch (random() % 4) {
      case 0:
        problem.rules.push_back(Rule::epsilon(lhs));
        break;
      case 1:
        problem.rules.push_back(Rule::unary(lhs, left));
        break;
      default:
        problem.rules.push_back(Rule::binary(lhs, left, right));
        break;
      }
    }

    for (PartitionMode mode : {PartitionMode::Grammar,
                               PartitionMode::Global,
                               PartitionMode::Singleton}) {
      for (bool factorized : {false, true}) {
        SCOPED_TRACE(trial);
        SCOPED_TRACE(static_cast<int>(mode));
        SCOPED_TRACE(factorized);
        Solver solver(problem, options(mode, factorized));
        solver.solve();
        expectMatchesOracle(solver, problem);
      }
    }
  }
}

TEST(EndpointQuotientAdvancedTest,
     IncrementalNodeGrowthAndPartitionRefinementMatchFreshOracle) {
  const std::vector<Rule> rules{
      Rule::unary(2, 0),      Rule::unary(3, 1),
      Rule::binary(4, 2, 3),  Rule::binary(4, 4, 4),
      Rule::binary(5, 4, 2),  Rule::binary(2, 3, 2),
      Rule::epsilon(3),
  };

  for (PartitionMode mode : {PartitionMode::Grammar,
                             PartitionMode::Singleton}) {
    for (bool factorized : {false, true}) {
      SCOPED_TRACE(static_cast<int>(mode));
      SCOPED_TRACE(factorized);
      const Options solver_options = options(mode, factorized);
      Problem problem{1, 6, {}, rules};
      auto snapshot = std::make_unique<Solver>(problem, solver_options);
      snapshot->solve();
      expectMatchesOracle(*snapshot, problem);

      for (Id round = 0; round < 14; ++round) {
        if (round % 2 == 0)
          ++problem.nodes;
        const Id n = problem.nodes;
        problem.edges.push_back(
            {(round * 3 + 1) % n, round % 2, (round * 5 + 2) % n});
        if (round % 3 == 0)
          problem.edges.push_back(
              {(round + 2) % n, (round + 1) % 2, (round * 2 + 1) % n});

        auto next =
            std::make_unique<Solver>(problem, *snapshot, solver_options);
        next->solve();
        SCOPED_TRACE(round);
        expectMatchesOracle(*next, problem);

        // The migrated result must equal a completely fresh saturation, not
        // merely the oracle's membership probes.
        Solver fresh(problem, solver_options);
        fresh.solve();
        std::set<Fact> incremental_facts;
        std::set<Fact> fresh_facts;
        next->forEachFact([&](Id symbol, Id source, Id target) {
          incremental_facts.emplace(symbol, source, target);
        });
        fresh.forEachFact([&](Id symbol, Id source, Id target) {
          fresh_facts.emplace(symbol, source, target);
        });
        EXPECT_EQ(incremental_facts, fresh_facts);
        snapshot = std::move(next);
      }
    }
  }
}

TEST(EndpointQuotientAdvancedTest,
     SelfRecursiveDeltaRowsCloseSameBatchAcrossSharedMiddles) {
  for (Id nodes : {2u, 3u, 7u, 17u, 65u}) {
    Problem problem{nodes, 2, {},
                    {Rule::unary(1, 0), Rule::binary(1, 1, 1)}};

    // All edges are seeded before the first pop, so every transitive result
    // depends on joining rows that initially belong to the same delta batch.
    for (Id source = 0; source < nodes; ++source) {
      problem.edges.push_back({source, 0, (source + 1) % nodes});
      if (source + 2 < nodes)
        problem.edges.push_back({source, 0, source + 2});
    }

    for (PartitionMode mode : {PartitionMode::Grammar,
                               PartitionMode::Singleton}) {
      for (bool factorized : {false, true}) {
        SCOPED_TRACE(nodes);
        SCOPED_TRACE(static_cast<int>(mode));
        SCOPED_TRACE(factorized);
        Solver solver(problem, options(mode, factorized));
        solver.solve();
        expectMatchesOracle(solver, problem);
        for (Id source = 0; source < nodes; ++source)
          for (Id target = 0; target < nodes; ++target)
            EXPECT_TRUE(solver.contains(1, source, target));
      }
    }
  }
}

} // namespace
} // namespace lotus::cfl::classical
