
#include "Checker/Pulse/PulseDisjunctiveDomain.h"

#include <algorithm>

namespace pulse {

static void reduceDisjuncts(std::vector<DisjunctiveDomain::Disjunct> &disjuncts,
                            size_t max) {
  if (disjuncts.size() <= max) {
    return;
  }

  std::vector<DisjunctiveDomain::Disjunct> selected;
  selected.reserve(max);
  std::set<const llvm::BasicBlock *> seen_ctx;

  for (auto &d : disjuncts) {
    if (selected.size() >= max) {
      break;
    }
    if (!d.path_context) {
      continue;
    }
    if (seen_ctx.insert(d.path_context).second) {
      selected.push_back(std::move(d));
    }
  }

  for (auto &d : disjuncts) {
    if (selected.size() >= max) {
      break;
    }
    selected.push_back(std::move(d));
  }

  disjuncts.swap(selected);
}

const std::vector<DisjunctiveDomain::Disjunct> &
DisjunctiveDomain::getDisjuncts(const llvm::BasicBlock *at_block) const {
  static const std::vector<DisjunctiveDomain::Disjunct> kEmpty;
  auto it = disjuncts_by_block_.find(at_block);
  return (it == disjuncts_by_block_.end()) ? kEmpty : it->second;
}

std::vector<DisjunctiveDomain::Disjunct> &
DisjunctiveDomain::getDisjuncts(const llvm::BasicBlock *at_block) {
  return disjuncts_by_block_[at_block];
}

size_t DisjunctiveDomain::size() const {
  size_t total = 0;
  for (const auto &kv : disjuncts_by_block_) {
    total += kv.second.size();
  }
  return total;
}

void DisjunctiveDomain::add(const llvm::BasicBlock *at_block,
                            ExecutionDomain state,
                            const llvm::BasicBlock *path_context) {
  disjuncts_by_block_[at_block].emplace_back(std::move(state), path_context);
  limitDisjuncts(at_block);
}

void DisjunctiveDomain::limitDisjuncts(const llvm::BasicBlock *at_block) {
  auto it = disjuncts_by_block_.find(at_block);
  if (it == disjuncts_by_block_.end()) {
    return;
  }
  reduceDisjuncts(it->second, kMaxDisjuncts);
}

ExecutionDomain DisjunctiveDomain::joinAtBlock(const llvm::BasicBlock *BB) {
  auto it = disjuncts_by_block_.find(BB);
  if (it == disjuncts_by_block_.end() || it->second.empty()) {
    return ExecutionDomain();
  }

  auto &disjuncts = it->second;
  if (disjuncts.size() == 1) {
    return disjuncts[0].state.clone();
  }

  std::vector<const AbductiveDomain *> astates;
  astates.reserve(disjuncts.size());
  for (const auto &disj : disjuncts) {
    if (disj.state.isStopped()) {
      continue;
    }
    if (const AbductiveDomain *a = disj.state.getAstate()) {
      astates.push_back(a);
    }
  }

  if (astates.empty()) {
    ExecutionDomain stopped;
    stopped.setState(ExecutionState::Stopped);
    return stopped;
  }

  // Over-approximate join for widening: try a precise merge; if contradictory,
  // fall back to a conservative join that drops path constraints.
  auto conservative_join = [](const AbductiveDomain &d1,
                              const AbductiveDomain &d2) -> AbductiveDomain {
    AbductiveDomain out = d1.clone();

    // Drop constraints to avoid under-approximation on contradictions.
    out.setPathFormula(std::make_unique<PulseFormula>());
    out.setUnknownValues(out.hasUnknownValues() || d2.hasUnknownValues());

    // Union post stack: keep existing bindings; add missing ones.
    for (const auto &kv : d2.getPostStack().getMap()) {
      if (!out.getPostStack().find(kv.first)) {
        out.getPostStack().add(kv.first, kv.second);
      }
    }

    // Union post heap edges.
    for (const auto &kv : d2.getPostHeap().getEdges()) {
      for (const auto &edge_kv : kv.second) {
        out.getPostHeap().addEdge(kv.first, edge_kv.first, edge_kv.second);
      }
    }

    // Union post attrs.
    for (const auto &kv : d2.getPostAttrs().getAttrs()) {
      for (Attribute attr : kv.second) {
        out.getPostAttrs().add(kv.first, attr);
      }
    }

    // Precondition: union similarly (best-effort).
    for (const auto &kv : d2.getPreStack().getMap()) {
      if (!out.getPreStack().find(kv.first)) {
        out.getPreStack().add(kv.first, kv.second);
      }
    }
    for (const auto &kv : d2.getPreHeap().getEdges()) {
      for (const auto &edge_kv : kv.second) {
        out.getPreHeap().addEdge(kv.first, edge_kv.first, edge_kv.second);
      }
    }
    for (const auto &kv : d2.getPreAttrs().getAttrs()) {
      for (Attribute attr : kv.second) {
        out.getPreAttrs().add(kv.first, attr);
      }
    }

    // Skipped calls: union
    for (const auto &name : d2.getSkippedCalls()) {
      out.addSkippedCall(name);
    }

    // Transitive info: conservative merge.
    out.setTransitiveInfo(
        TransitiveInfo::merge(out.getTransitiveInfo(), d2.getTransitiveInfo()));
    out.addRecursiveCalls(d2.getRecursiveCalls());
    for (AbstractValue av : d2.getNeedDynamicTypeSpecialization()) {
      out.addNeedDynamicTypeSpecialization(av);
    }

    out.canonicalize();
    return out;
  };

  AbductiveDomain merged = astates[0]->clone();
  for (size_t i = 1; i < astates.size(); ++i) {
    auto precise = AbductiveDomain::merge(merged, *astates[i]);
    if (precise) {
      merged = precise->clone();
    } else {
      merged = conservative_join(merged, *astates[i]);
    }
  }

  return ExecutionDomain(std::make_unique<AbductiveDomain>(std::move(merged)));
}

bool DisjunctiveDomain::shouldWiden(const llvm::BasicBlock *BB) const {
  auto it = block_iterations_.find(BB);
  if (it == block_iterations_.end()) {
    return false;
  }
  return it->second >= kWidenThreshold;
}

void DisjunctiveDomain::widen(const llvm::BasicBlock *BB) {
  auto it = block_iterations_.find(BB);
  if (it == block_iterations_.end()) {
    block_iterations_[BB] = 1;
  } else {
    it->second++;
  }

  // Apply widening if threshold reached
  if (block_iterations_[BB] >= kWidenThreshold) {
    auto it = disjuncts_by_block_.find(BB);
    if (it != disjuncts_by_block_.end()) {
      reduceDisjuncts(it->second, kWidenKeepDisjuncts);
    }
  }
}

} // namespace pulse
