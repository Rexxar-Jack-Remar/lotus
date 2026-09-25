#include "Dataflow/APA/Tooling/APADiagnostics.h"

#include <algorithm>

using namespace llvm;

namespace elimination::tooling {

CFGStats collectCFGStats(const Function &F) {
  CFGStats Stats;
  Stats.Arguments = F.arg_size();
  for (const auto &BB : F) {
    ++Stats.Blocks;
    Stats.Instructions += BB.size();
    for (const auto &I : BB) {
      if (isa<PHINode>(I))
        ++Stats.PhiNodes;
      if (isa<CallBase>(I))
        ++Stats.Calls;
      if (isa<ReturnInst>(I))
        ++Stats.Returns;
      if (isa<UnreachableInst>(I))
        ++Stats.Unreachable;
    }
    const size_t Succs = succ_size(&BB);
    Stats.Edges += Succs;
    Stats.MaxSuccessors = std::max(Stats.MaxSuccessors, Succs);
    if (Succs > 1)
      ++Stats.BranchingBlocks;
  }
  return Stats;
}

void emitOrderingDiagnostics(raw_ostream &OS,
                             const elimination::OrderingDiagnostics &D) {
  if (D.regions == 0) {
    return;
  }
  OS << "  [ordering] regions=" << D.regions << ", pivots=" << D.selected_nodes
     << ", refreshes=" << D.score_refreshes
     << ", stale=" << D.stale_heap_entries
     << ", allocated=" << D.allocated_nodes
     << ", initial_allocated=" << D.initial_allocated_nodes
     << ", elimination_allocated=" << D.elimination_allocated_nodes
     << ", backsubstitution_allocated=" << D.backsubstitution_allocated_nodes
     << ", boundary_allocated=" << D.boundary_allocated_nodes
     << ", star_allocated=" << D.allocated_stars << ", bypasses=" << D.bypasses
     << ", peak_live_nodes=" << D.peak_live_nodes
     << ", peak_live_edges=" << D.peak_live_edges
     << ", peak_active_nodes=" << D.peak_active_nodes
     << ", peak_active_edges=" << D.peak_active_edges
     << ", metadata_ns=" << D.metadata_time_ns
     << ", scoring_ns=" << D.scoring_time_ns << ", heap_ns=" << D.heap_time_ns
     << ", ranking_ns=" << D.metadata_time_ns + D.selection_time_ns << "\n";
  for (const auto &S : D.score_updates) {
    OS << "  [order-score] region=" << S.region << ", step=" << S.step
       << ", node=" << S.node << ", version=" << S.version
       << ", structural=" << S.signals.structural
       << ", expression=" << S.signals.expression << ", star=" << S.signals.star
       << ", score=" << S.score << "\n";
  }
  for (const auto &S : D.trace) {
    OS << "  [order-step] region=" << S.region << ", step=" << S.step
       << ", node=" << S.node << ", structural=" << S.signals.structural
       << ", expression=" << S.signals.expression << ", star=" << S.signals.star
       << ", score=" << S.score << ", allocated=" << S.allocated_nodes
       << ", star_allocated=" << S.allocated_stars
       << ", live_nodes=" << S.live_nodes << ", live_edges=" << S.live_edges
       << ", active_nodes=" << S.active_nodes
       << ", active_edges=" << S.active_edges << ", bypasses=" << S.bypasses
       << "\n";
  }
}

}
