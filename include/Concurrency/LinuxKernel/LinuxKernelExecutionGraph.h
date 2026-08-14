/**
 * @file LinuxKernelExecutionGraph.h
 * @brief Whole-program execution contexts for Linux kernel callbacks.
 */

#pragma once

#include "Concurrency/LinuxKernel/LinuxKernelOperation.h"

#include <cstddef>
#include <map>
#include <set>
#include <vector>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace lotus {
class HappensBeforeAnalysis;
} // namespace lotus

namespace mhp {
class IMHPAnalysis;
} // namespace mhp

namespace kernel {

class LinuxKernelProcessModel;

class LinuxKernelExecutionGraph {
public:
  using ContextID = std::size_t;

  enum class EdgeKind {
    REGISTER,
    SPAWN,
    SUBMIT,
    CANCEL,
    FLUSH,
    JOIN,
  };

  struct Context {
    ContextID id = 0;
    AsyncContextKind kind = AsyncContextKind::NONE;
    const llvm::Function *entry = nullptr;
    const llvm::Instruction *origin = nullptr;
    const llvm::Value *object = nullptr;
    const llvm::Value *serialization_domain = nullptr;
    bool serializes_domain = false;
    bool explicit_concurrency = false;
  };

  struct Edge {
    EdgeKind kind = EdgeKind::REGISTER;
    const llvm::Instruction *operation = nullptr;
    ContextID context = 0;
    bool synchronous = false;
  };

  explicit LinuxKernelExecutionGraph(const LinuxKernelProcessModel &model)
      : process_model_(model) {}

  void setMHPAnalysis(const mhp::IMHPAnalysis *mhp) { mhp_ = mhp; }
  void setHappensBeforeAnalysis(const lotus::HappensBeforeAnalysis *hb) {
    happens_before_ = hb;
  }

  void analyze();

  const std::vector<Context> &getContexts() const { return contexts_; }
  const std::vector<Edge> &getEdges() const { return edges_; }

  std::vector<const Context *>
  getContextsForFunction(const llvm::Function *function) const;

  bool hasExplicitConcurrencyEvidence(const llvm::Instruction *lhs,
                                      const llvm::Instruction *rhs) const;
  bool mayHappenInParallel(const llvm::Instruction *lhs,
                           const llvm::Instruction *rhs) const;
  bool happensBefore(const llvm::Instruction *lhs,
                     const llvm::Instruction *rhs) const;

private:
  const LinuxKernelProcessModel &process_model_;
  const mhp::IMHPAnalysis *mhp_ = nullptr;
  const lotus::HappensBeforeAnalysis *happens_before_ = nullptr;

  std::vector<Context> contexts_;
  std::vector<Edge> edges_;
  std::map<const llvm::Function *, std::set<ContextID>> function_contexts_;

  bool originMayPrecede(const llvm::Instruction *origin,
                        const llvm::Instruction *instruction) const;
  bool contextsMayOverlap(const Context &lhs, const Context &rhs) const;
};

} // namespace kernel
