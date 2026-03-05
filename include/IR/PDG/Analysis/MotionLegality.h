/**
 * @file MotionLegality.h
 * @brief Code-motion legality queries over the Program Dependency Graph (PDG).
 *
 * This file provides conservative legality checks for moving a node earlier
 * (hoisting) or later (sinking) relative to an anchor node. The analysis is
 * intended as a production-grade legality oracle for optimization passes such
 * as LICM, sinking, and local scheduling.
 */

#pragma once

#include "IR/PDG/Core/Graph.h"
#include "IR/PDG/Core/PDGEdge.h"
#include "IR/PDG/Core/PDGEnums.h"
#include "IR/PDG/Core/PDGNode.h"

#include <set>
#include <string>
#include <vector>

namespace pdg {

enum class MotionDirection { Earlier, Later };

struct MotionLegalityPolicy {
  /// Relevant edge kinds for dependence legality. Empty = defaults.
  std::set<EdgeType> edge_types;

  /// Require moving node and anchor to be in the same LLVM function.
  bool require_same_function = true;

  /// Enforce control-context compatibility checks.
  bool respect_control_dependence = true;

  /// Forbid moving potentially side-effecting or trapping instructions.
  bool allow_side_effecting_instructions = false;

  /// Allow speculative motion through weaker control context.
  bool allow_speculation = false;
};

struct MotionLegalityResult {
  bool legal = false;
  MotionDirection direction = MotionDirection::Earlier;

  Node *moving_node = nullptr;
  Node *anchor_node = nullptr;

  /// Dependence witness path blocking motion (if any).
  std::vector<Node *> blocking_path;

  /// Edge types observed on the blocking path.
  std::vector<EdgeType> blocking_edge_types;

  /// Human-readable reason for diagnostics/logging.
  std::string reason;
};

class MotionLegalityQuery {
public:
  explicit MotionLegalityQuery(GenericGraph &pdg) : _pdg(pdg) {}

  MotionLegalityResult canMoveEarlier(Node &moving_node, Node &anchor_node,
                                      const MotionLegalityPolicy &policy = {});

  MotionLegalityResult canMoveLater(Node &moving_node, Node &anchor_node,
                                    const MotionLegalityPolicy &policy = {});

private:
  GenericGraph &_pdg;

  static std::set<EdgeType> defaultMotionEdgeTypes();
  static std::set<EdgeType> controlEdgeTypes();

  MotionLegalityResult runCheck(Node &moving_node, Node &anchor_node,
                                MotionDirection direction,
                                const MotionLegalityPolicy &policy);

  bool isMovableInstruction(Node &node, const MotionLegalityPolicy &policy,
                            std::string &reason) const;

  bool findPath(Node &source, Node &target,
                const std::set<EdgeType> &edge_types, std::vector<Node *> &path,
                std::vector<EdgeType> &path_edge_types) const;

  std::set<Node *> collectControllers(Node &node) const;
};

} // namespace pdg
