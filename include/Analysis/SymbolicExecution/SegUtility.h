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

class ExternalMemorySpecCompat {
public:
  std::vector<int> getHeapAllocSize(const llvm::CallBase *CB) const;
  bool isPureLib(const llvm::Function *F) const;
};

void initAnalysisInterface(
    llvm::Module *M, const llvm::DataLayout *DL,
    SymbolicExecution::GuardedValueFlowBuilderPass *builder);

TaintModel *getTaintSpec();

SymbolicExecution::GuardedValueFlowGraph *getGraph(llvm::Function *func);

std::vector<std::pair<SymbolicExecution::GuardedValueFlowNode *,
                      SymbolicExecution::GuardedValueFlowRegionNode *>>
getIncomingValuesForLoad(const SymbolicExecution::GuardedValueFlowNode *node);

llvm::Function *getEnclosingFunc(SymbolicExecution::Var v);
llvm::Function *getCallee(llvm::Instruction *I);
bool isDefiniteCall(llvm::Instruction *I);
bool isMatchLib(llvm::CallInst *CallI, const std::string &callee_name,
                const std::string &lib_name);
bool isKnownLib(const std::string &lib_name);
std::set<SymbolicExecution::Var> getTaintedVars(llvm::Instruction *I,
                                                TaintModel *TaintSpec);

void getTopoOrder(llvm::Module &M);
const std::vector<llvm::Function *> &getFuncSeq();
unsigned getFunctionDepth(llvm::Function *func);
bool isFunctionTopLevel(llvm::Function *func);

ExternalMemorySpecCompat *getMemSpec();

size_t hashHelper(const std::vector<size_t> &hash_vals);
std::string ptrToString(const void *ptr);

const llvm::DataLayout *getDL();
uint64_t getTypeSizeInBits(llvm::Type *Ty);
uint64_t getTypeStoreSize(llvm::Type *Ty);
uint64_t getTypeStoreSizeInBits(llvm::Type *Ty);
uint64_t getElementOffset(llvm::StructType *St, unsigned Idx);

template <typename T> llvm::Value *getPointerOperand(T *V) {
  return V->getPointerOperand()->stripPointerCasts();
}

} // namespace seg_utility
