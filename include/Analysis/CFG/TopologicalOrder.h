/** @file TopologicalOrder.h @brief Topological order computation for CFGs. */
#ifndef __TOPOLOGICAL_ORDER__HH_
#define __TOPOLOGICAL_ORDER__HH_

#include "llvm/IR/Function.h"
#include "llvm/Pass.h"

#include <vector>

/// Constructs an entry-reachable topological order after removing back-edges.
/// Declarations produce an empty order; unreachable blocks are not included.
class TopologicalOrder : public llvm::FunctionPass {

  llvm::SmallVector<
      std::pair<const llvm::BasicBlock *, const llvm::BasicBlock *>, 16>
      m_backEdges;

  using BlockVector = std::vector<const llvm::BasicBlock *>;
  BlockVector m_order;

public:
  static char ID;

  TopologicalOrder() : FunctionPass(ID) {}

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  virtual void releaseMemory() override {
    m_order.clear();
    m_backEdges.clear();
  }

  bool isBackEdge(const llvm::BasicBlock &src,
                  const llvm::BasicBlock &dst) const;

  using iterator = BlockVector::iterator;
  using const_iterator = BlockVector::const_iterator;
  using reverse_iterator = BlockVector::reverse_iterator;
  using const_reverse_iterator = BlockVector::const_reverse_iterator;

  iterator begin() { return m_order.begin(); }
  iterator end() { return m_order.end(); }
  llvm::iterator_range<iterator> topoOrder() {
    return llvm::make_range(begin(), end());
  }
  const_iterator begin() const { return m_order.begin(); }
  const_iterator end() const { return m_order.end(); }
  llvm::iterator_range<const_iterator> topoOrder() const {
    return llvm::make_range(begin(), end());
  }
  reverse_iterator rbegin() { return m_order.rbegin(); }
  reverse_iterator rend() { return m_order.rend(); }
  llvm::iterator_range<reverse_iterator> rtopoOrder() {
    return llvm::make_range(rbegin(), rend());
  }
  const_reverse_iterator rbegin() const { return m_order.rbegin(); }
  const_reverse_iterator rend() const { return m_order.rend(); }
  llvm::iterator_range<const_reverse_iterator> rtopoOrder() const {
    return llvm::make_range(rbegin(), rend());
  }

  void print(llvm::raw_ostream &out, const llvm::Module *m) const override;
  llvm::StringRef getPassName() const override { return "TopologicalOrder"; }
};

#endif
