// SPDX-License-Identifier: MIT
#pragma once
// Deliberately NOT installed into Lotus. Test-enabled builds compile the
// observer hooks into CanaryDynamicDyck; normal builds omit them.
#include "CFL/DynamicDyck/PrimaryComponent/PrimaryComponentSolver.h"

#include <functional>
#include <utility>

namespace lotus::cfl::dynamic_dyck::primary_component {
enum class TestPhase {
  Boundary,
  InsertionPrepared,
  AffectedDiscovered,
  DeletionPrepared
};
struct TestSnapshot {
  using Node = std::size_t;
  struct List {
    Node source;
    Label label;
    std::vector<Node> targets;
  };
  TestPhase phase;
  std::vector<Vertex> vertices;
  std::vector<Node> parent, dscc, primary, affected;
  std::vector<List> out_edges, in_primary, summaries;
  std::vector<std::pair<Node, Label>> queue;
};
struct TestAccess {
  using Observer = std::function<void(const TestSnapshot &)>;
  static void observe(PrimaryComponentSolver &solver, Observer observer);
  static TestSnapshot snapshot(const PrimaryComponentSolver &solver);
};
} // namespace lotus::cfl::dynamic_dyck::primary_component
