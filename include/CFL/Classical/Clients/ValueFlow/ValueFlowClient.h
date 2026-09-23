#pragma once

#include "CFL/Classical/Clients/ValueFlow/SVFGPreparation.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lotus::analysis {
class SVFG;
} // namespace lotus::analysis

namespace lotus::cfl::classical {

enum class ValueFlowEncodingMode {
  /// Lotus-native balanced/realizable value-flow relations, including edge
  /// kinds and their reverse labels.
  Native,
  /// Classical CFL interchange encoding: a, call_i, and ret_i.
  ClassicalCFL,
};

LabeledGraph encodeSVFG(const lotus::analysis::SVFG &svfg);
Grammar buildVfgGrammar(const lotus::analysis::SVFG &svfg);
LabeledGraph encodeClassicalCflSVFG(const lotus::analysis::SVFG &svfg);
Grammar buildClassicalCflVfgGrammar(const lotus::analysis::SVFG &svfg);

/// Context-sensitive may-reach value-flow facade. Direct, indirect-memory,
/// and may-happen-in-parallel input edges remain distinguishable in the
/// encoded graph. Balanced summaries and general realizable paths are exposed
/// separately.
class ValueFlowClient {
public:
  ~ValueFlowClient();
  ValueFlowClient(ValueFlowClient &&other) noexcept;
  ValueFlowClient &operator=(ValueFlowClient &&other) noexcept;
  ValueFlowClient(const ValueFlowClient &) = delete;
  ValueFlowClient &operator=(const ValueFlowClient &) = delete;

  static ValueFlowClient
  fromSVFG(const lotus::analysis::SVFG &svfg,
           ValueFlowEncodingMode mode = ValueFlowEncodingMode::Native);
  static ValueFlowClient
  fromPreparedSVFG(lotus::analysis::SVFG &svfg,
                   const SVFGPreparationOptions &options = {},
                   ValueFlowEncodingMode mode = ValueFlowEncodingMode::Native);

  ReachabilityStats solve(SolverBackend backend = SolverBackend::SparseSet);
  /// Backward-compatible spelling for hasBalancedFlow().
  bool hasFlow(std::uint32_t source_node, std::uint32_t target_node) const;
  bool hasBalancedFlow(std::uint32_t source_node,
                       std::uint32_t target_node) const;
  /// Allow unmatched returns at the path beginning and unmatched calls at the
  /// path end while preserving matching for every balanced call/return pair.
  bool hasRealizableFlow(std::uint32_t source_node,
                         std::uint32_t target_node) const;
  /// Backward-compatible balanced-summary reachability enumeration.
  std::vector<std::uint32_t> reachableFrom(std::uint32_t source_node) const;
  std::vector<std::uint32_t>
  realizableReachableFrom(std::uint32_t source_node) const;

  const LabeledGraph &graph() const;
  const Grammar &grammar() const;

private:
  ValueFlowClient(
      LabeledGraph graph, Grammar grammar,
      std::unordered_map<std::uint32_t, std::size_t> node_to_vertex);

  struct State;
  std::unique_ptr<State> state_;
  std::unordered_map<std::uint32_t, std::size_t> node_to_vertex_;
  std::vector<std::optional<std::uint32_t>> vertex_to_node_;
  std::unique_ptr<SolverSession> session_;
  std::optional<SolverBackend> backend_;

  bool contains(std::uint32_t source_node, std::uint32_t target_node,
                const char *symbol) const;
  std::vector<std::uint32_t> reachableFromSymbol(std::uint32_t source_node,
                                                 const char *symbol) const;
};

} // namespace lotus::cfl::classical
