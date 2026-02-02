#ifndef DATAFLOW_ELIMINATION_ELIMINATIONFRAMEWORK_H_
#define DATAFLOW_ELIMINATION_ELIMINATIONFRAMEWORK_H_

#include <cstddef>
#include <utility>
#include <vector>

namespace elimination {

template <typename AnalysisDomainTy> class IntraEliminationProblem {
public:
  using n_t = typename AnalysisDomainTy::n_t;
  using fact_t = typename AnalysisDomainTy::fact_t;
  using transfer_t = typename AnalysisDomainTy::transfer_t;

  virtual ~IntraEliminationProblem() = default;

  // Graph
  virtual std::vector<n_t> nodes() const = 0;
  virtual n_t entry() const = 0;
  virtual std::vector<n_t> succs(n_t Node) const = 0;

  // Edge semantics
  virtual transfer_t edgeTransfer(n_t Src, n_t Dst) const = 0;
  virtual fact_t applyTransfer(const transfer_t &T, const fact_t &In) const = 0;

  // Meet operator (used as "combine over paths" for MOP-style evaluation).
  virtual fact_t meet(const fact_t &Lhs, const fact_t &Rhs) const = 0;
  virtual bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const = 0;

  // Neutral element for meet over an empty set of paths.
  virtual fact_t meetIdentity() const = 0;

  // Fact at the entry before traversing any edge.
  virtual fact_t initialFact() const = 0;

  // Star evaluation needs a convergence guard for non-finite lattices.
  virtual std::size_t maxStarIterations() const { return 100000; }
};

// Optional extension for "paper-style" ADT-based elimination on reducible flowgraphs.
// The ADT solver will use this information; otherwise it falls back to StateElimination.
template <typename AnalysisDomainTy>
class IntraReducibleEliminationProblem
    : public IntraEliminationProblem<AnalysisDomainTy> {
public:
  using Base = IntraEliminationProblem<AnalysisDomainTy>;
  using n_t = typename Base::n_t;

  struct Edge final {
    n_t Src{};
    n_t Dst{};
  };

  // Enumerate edges of the flowgraph.
  virtual std::vector<Edge> edges() const = 0;

  // Total order over nodes such that forward edges go from lower to higher index.
  // (Back edges may violate the order.)
  virtual std::vector<n_t> topologicalOrder() const = 0;

  // Immediate dominator (idom(entry) may equal entry).
  virtual n_t idom(n_t Node) const = 0;

  // Dominance query (A dominates B).
  virtual bool dominates(n_t A, n_t B) const = 0;

  // Default back-edge predicate from the paper: destination dominates source.
  virtual bool isBackEdge(n_t Src, n_t Dst) const { return dominates(Dst, Src); }
};

} // namespace elimination

#endif // DATAFLOW_ELIMINATION_ELIMINATIONFRAMEWORK_H_
