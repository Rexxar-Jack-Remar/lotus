#ifndef STATIC_THREAD_SHARING_ANALYSIS_H
#define STATIC_THREAD_SHARING_ANALYSIS_H

#include "llvm/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace seadsa {
  class Graph;
  class Node;
  class Cell;
  class GlobalAnalysis;
} // namespace seadsa

namespace lotus {

class StaticThreadSharingAnalysis : public llvm::ModulePass {
public:
  static char ID;
  StaticThreadSharingAnalysis();

  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  llvm::StringRef getPassName() const override { return "Static Thread Sharing Analysis"; }

  /// Returns true if the memory accessed by this instruction is shared among threads
  bool isShared(const llvm::Instruction *Inst) const;
  
  /// Returns true if the object (represented by allocation site) is shared
  bool isShared(const llvm::Value *AllocSite) const;

private:
  struct AccessInfo {
    std::set<const llvm::Function*> Readers; // Thread Entry Functions
    std::set<const llvm::Function*> Writers; // Thread Entry Functions
  };

  // Map from Allocation Site -> Field Offset -> AccessInfo
  // Field Offset -1 means "array" or "whole object" or "unknown"
  using FieldAccessMap = std::map<int, AccessInfo>;
  using AllocAccessMap = std::unordered_map<const llvm::Value*, FieldAccessMap>;

  AllocAccessMap m_allocAccesses;
  
  // Reference to SeaDSA analysis result
  seadsa::GlobalAnalysis *m_dsa;

  // Set of thread entry functions
  std::vector<const llvm::Function*> m_threads;
  
  void findStaticThreads(llvm::Module &M);
  void visitThread(const llvm::Function *ThreadEntry);
  void visitMethod(const llvm::Function *F, const llvm::Function *ThreadEntry, 
                   std::set<const llvm::Function*> &Visited);
  
  void recordAccess(const llvm::Instruction *Inst, bool isWrite, const llvm::Function *ThreadEntry, seadsa::Graph &G);
  
  // Helper to check if a thread runs multiple times (heuristic)
  bool isMultiRunThread(const llvm::Function *ThreadEntry) const;
};

} // namespace lotus

#endif // STATIC_THREAD_SHARING_ANALYSIS_H

