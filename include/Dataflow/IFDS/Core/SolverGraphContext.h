#pragma once

#include "Dataflow/IFDS/Core/AnalysisSession.h"

#include <stdexcept>

namespace ifds {

template <typename Fact, typename Problem> class SolverGraphContext {
public:
  using InitialSeeds = typename IFDSProblem<Fact>::InitialSeeds;

  void set_icfg(std::shared_ptr<dataflow::controlflow::InterCFG> icfg) {
    m_icfg = std::move(icfg);
  }
  void set_callee_provider(CalleeProvider provider) {
    m_provider = std::move(provider);
  }
  void initialize(const llvm::Module &module,
                  std::shared_ptr<AnalysisSession> session = {}) {
    if (session && &session->module() != &module)
      throw std::invalid_argument(
          "Analysis session belongs to a different module");
    if (session && !m_icfg && !m_provider) {
      m_graph = session->graph();
    } else {
      lotus::SparseLLVMControlFlow::AliasCheckFn may_alias;
      if (session) {
        auto aliases = session->alias_analysis();
        may_alias = [aliases](const llvm::Value *left,
                              const llvm::Value *right) {
          return aliases->mayAlias(left, right);
        };
      }
      m_graph = std::make_shared<AnalysisGraph>(
          const_cast<llvm::Module &>(module), m_icfg, m_provider, may_alias);
    }
  }

  std::vector<const llvm::Instruction *>
  get_return_sites(const llvm::CallBase *call) const {
    return m_graph->return_sites(call);
  }
  std::vector<const llvm::Instruction *>
  get_successors(const llvm::Instruction *inst) const {
    return m_graph->successors(inst);
  }

  // Sparse CFG is a hint; the client must certify each skipped transfer.
  // Preserve calls, terminators, block entries and injected CFG boundaries.
  template <typename IsIdentity>
  const llvm::Instruction *sparse_next(const llvm::Instruction *inst,
                                       const llvm::Value *value,
                                       IsIdentity is_identity) const {
    if (!value || !inst || llvm::isa<llvm::CallBase>(inst) ||
        inst->isTerminator() || &inst->getParent()->front() == inst)
      return nullptr;
    auto *next = inst->getNextNode();
    const auto &successors = m_graph->successors(inst);
    if (!next || successors.size() != 1 || successors.front() != next)
      return nullptr;
    if (m_graph->sparse_cfg().shouldKeepInst(inst, value) ||
        !is_identity(inst, next))
      return nullptr;
    return next;
  }
  InitialSeeds build_initial_seeds(Problem &problem,
                                   const llvm::Module &module) const {
    return problem.initial_seeds(module);
  }
  void clear() { m_graph.reset(); }
  const auto &call_to_callees() const { return m_graph->callees(); }
  const auto &callee_to_calls() const { return m_graph->callers(); }

private:
  std::shared_ptr<dataflow::controlflow::InterCFG> m_icfg;
  CalleeProvider m_provider;
  std::shared_ptr<const AnalysisGraph> m_graph;
};
} // namespace ifds
