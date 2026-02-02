#ifndef DATAFLOW_ELIMINATION_OPTIONS_H_
#define DATAFLOW_ELIMINATION_OPTIONS_H_

namespace elimination {

enum class EliminationMethod {
  // Generic O(n^3) state-elimination (Floyd–Warshall-style) over all nodes.
  StateElimination,
  // Paper-style ADT + path-expression construction (requires reducible info).
  ADTDelayed,
};

struct EliminationOptions final {
  EliminationMethod Method = EliminationMethod::StateElimination;
};

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_OPTIONS_H_

