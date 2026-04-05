#pragma once

#include "IR/GVFG/GuardedValueFlowBuilder.h"

#include <string>
#include <vector>

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

namespace SymbolicExecution {
class Var;
using lotus::gvfg::GuardedValueFlowBuilderPass;
using lotus::gvfg::GuardedValueFlowGraph;
using lotus::gvfg::GuardedValueFlowNode;
using lotus::gvfg::GuardedValueFlowRegionNode;
} // namespace SymbolicExecution

class TaintModel;

namespace seg_utility {

/// Compatibility layer for external-memory summaries used by symbolic memory
/// modeling.
///
/// The symbolic executor relies on a mix of Lotus analyses and library-specific
/// knowledge. This shim exposes the small set of allocator and purity queries
/// that the engine needs without requiring every caller to know where that
/// information originates.
class ExternalMemorySpecCompat {
public:
  std::vector<int> getHeapAllocSize(const llvm::CallBase *CB) const;
  bool isPureLib(const llvm::Function *F) const;
};

/// Initializes shared module-level state used by the symbolic execution stack.
///
/// Clients typically call this once before analyzing functions so later helper
/// queries can recover the active module, data layout, and GVFG builder.
void initAnalysisInterface(
    llvm::Module *M, const llvm::DataLayout *DL,
    SymbolicExecution::GuardedValueFlowBuilderPass *builder);

/// Returns the process-wide taint specification consulted by transfer logic.
TaintModel *getTaintSpec();

/// Returns the GVFG for the given function.
/// The symbolic executor uses the guarded value-flow graph as its main program
/// representation when traversing loads, stores, phis, and call boundaries.
SymbolicExecution::GuardedValueFlowGraph *getGraph(llvm::Function *func);

std::vector<std::pair<SymbolicExecution::GuardedValueFlowNode *,
                      SymbolicExecution::GuardedValueFlowRegionNode *>>
getIncomingValuesForLoad(const SymbolicExecution::GuardedValueFlowNode *node);

llvm::Function *getEnclosingFunc(const SymbolicExecution::Var &v);
llvm::Function *getCallee(llvm::Instruction *I);
bool isDefiniteCall(llvm::Instruction *I);

/// Library-identification helpers shared by taint rules and memory modeling.
bool isMatchLib(llvm::CallInst *CallI, const std::string &callee_name,
                const std::string &lib_name);
bool isKnownLib(const std::string &lib_name);

/// Applies the taint specification at one instruction and returns the symbolic
/// variables that should enter the taint state as sources or propagated facts.
std::set<SymbolicExecution::Var> getTaintedVars(llvm::Instruction *I,
                                                TaintModel *TaintSpec);

/// Computes and exposes a module-level topological order over functions.
/// Summary construction and interprocedural scheduling use this order to visit
/// callees before callers when possible.
void getTopoOrder(llvm::Module &M);
const std::vector<llvm::Function *> &getFuncSeq();
unsigned getFunctionDepth(llvm::Function *func);
bool isFunctionTopLevel(llvm::Function *func);

/// Returns the external-memory helper used by allocation modeling.
ExternalMemorySpecCompat *getMemSpec();

/// Shared hashing and debug helpers used by symbolic-state keys.
size_t hashHelper(const std::vector<size_t> &hash_vals);
std::string ptrToString(const void *ptr);

/// Data-layout queries forwarded through the initialized module context.
const llvm::DataLayout *getDL();
uint64_t getTypeSizeInBits(llvm::Type *Ty);
uint64_t getTypeStoreSize(llvm::Type *Ty);
uint64_t getTypeStoreSizeInBits(llvm::Type *Ty);
uint64_t getElementOffset(llvm::StructType *St, unsigned Idx);

template <typename T> llvm::Value *getPointerOperand(T *V) {
  return V->getPointerOperand()->stripPointerCasts();
}

} // namespace seg_utility
