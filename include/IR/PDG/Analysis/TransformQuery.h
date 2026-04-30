/**
 * @file TransformQuery.h
 * @brief Dependence-aware transform legality and scheduling queries over the PDG.
 */

#pragma once

#include "IR/PDG/Analysis/QueryCore.h"

namespace pdg {

/// Dependence-aware transform legality and scheduling helpers.
class TransformQuery {
public:
  explicit TransformQuery(ProgramGraph &pdg) : pdg_(pdg) {}

  MotionCheckResult canMoveEarlier(Node &moving_node, Node &anchor_node,
                                   const LLVMQueryContext &llvm_context,
                                   const PDGQueryOptions &options =
                                       PDGQueryOptions()) const;

  MotionCheckResult canMoveLater(Node &moving_node, Node &anchor_node,
                                 const LLVMQueryContext &llvm_context,
                                 const PDGQueryOptions &options =
                                     PDGQueryOptions()) const;

  IndependenceCheckResult independent(
      Node &a, Node &b, const LLVMQueryContext &llvm_context,
      const PDGQueryOptions &options = PDGQueryOptions()) const;

  PDGQueryResult readySet(const PDGQueryScope &scope,
                          const PDGQueryResult::NodeSet &scheduled,
                          const LLVMQueryContext &llvm_context,
                          const PDGQueryOptions &options =
                              PDGQueryOptions()) const;

  std::vector<PDGQueryResult::NodeSet>
  topologicalLevels(const PDGQueryScope &scope,
                    const LLVMQueryContext &llvm_context,
                    const PDGQueryOptions &options =
                        PDGQueryOptions()) const;

  std::vector<PDGQueryResult::NodeSet>
  stronglyConnectedComponents(const PDGQueryScope &scope,
                              const LLVMQueryContext &llvm_context,
                              const PDGQueryOptions &options =
                                  PDGQueryOptions()) const;

  size_t criticalPathLength(const PDGQueryScope &scope,
                            const LLVMQueryContext &llvm_context,
                            const PDGQueryOptions &options =
                                PDGQueryOptions()) const;

private:
  ProgramGraph &pdg_;
};

} // namespace pdg
