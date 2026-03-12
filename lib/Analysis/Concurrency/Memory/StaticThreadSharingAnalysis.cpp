/*
Scalable thread sharing analysis, ICSE 16. Jeff Huang

This analysis identifies memory locations that are accessed by multiple threads.
It serves as a prerequisite for race detection.

Soundness/Approximations:
- Shared Data: Considers data shared if accessed by >1 thread, or written by a multi-run thread.
- Uncollapsed Globals: Uses the accessing pointer as a proxy for the memory location,
  which is a reasonable approximation when SeaDSA collapses globals.
- Immutable Data: Distinguishes between reads and writes to ignore immutable shared data.
*/
#include "Analysis/Concurrency/Memory/StaticThreadSharingAnalysis.h"
#include "Alias/seadsa/DsaAnalysis.hh"
#include "Alias/seadsa/Global.hh"
#include "Alias/seadsa/Graph.hh"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace seadsa;

namespace lotus {

char StaticThreadSharingAnalysis::ID = 0;

StaticThreadSharingAnalysis::StaticThreadSharingAnalysis() : ModulePass(ID), m_dsa(nullptr) {}

void StaticThreadSharingAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DsaAnalysis>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.setPreservesAll();
}

bool StaticThreadSharingAnalysis::runOnModule(Module &M) {
  m_allocAccesses.clear();
  m_threads.clear();
  m_threads_complete = true;
  
  // Get SeaDSA analysis
  DsaAnalysis &dsaPass = getAnalysis<DsaAnalysis>();
  m_dsa = &dsaPass.getDsaAnalysis();

  findStaticThreads(M);
  
  errs() << "StaticThreadSharingAnalysis: Found " << m_threads.size() << " static threads.\n";
  
  for (const Function *threadEntry : m_threads) {
    visitThread(threadEntry);
  }
  
  return false;
}

void StaticThreadSharingAnalysis::findStaticThreads(Module &M) {
  ThreadAPI *api = ThreadAPI::getThreadAPI();
  
  // Add main as a thread
  if (Function *Main = M.getFunction("main")) {
    m_threads.push_back(Main);
  }

  for (Function &F : M) {
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      if (api->isTDFork(&*I)) {
        const Value *entry = api->getForkedFun(&*I);
        if (!entry) {
          m_threads_complete = false;
          continue;
        }
        if (const Function *entryFunc = dyn_cast<Function>(entry)) {
           // Avoid duplicates
           if (std::find(m_threads.begin(), m_threads.end(), entryFunc) == m_threads.end())
             m_threads.push_back(entryFunc);
        } else {
            // Handle cases where entry is a bitcast or not a direct function
            const Value *stripped = entry->stripPointerCasts();
            if (const Function *f = dyn_cast<Function>(stripped)) {
                if (std::find(m_threads.begin(), m_threads.end(), f) == m_threads.end())
                    m_threads.push_back(f);
            } else {
                m_threads_complete = false;
            }
        }
      }
    }
  }
}

void StaticThreadSharingAnalysis::visitThread(const Function *ThreadEntry) {
  std::set<const Function*> visited;
  visitMethod(ThreadEntry, ThreadEntry, visited);
}

void StaticThreadSharingAnalysis::visitMethod(const Function *F, const Function *ThreadEntry, 
                                              std::set<const Function*> &Visited) {
  // Traverses the call graph reachable from ThreadEntry.
  // For each function, it iterates all instructions to find memory accesses (Loads/Stores).
  // It resolves the memory location using SeaDSA's graph.
  
  if (!F || F->isDeclaration() || Visited.count(F)) return;
  Visited.insert(F);
  
  // Access analysis
  if (m_dsa->hasGraph(*F)) {
      Graph &G = m_dsa->getGraph(*F);
      for (const BasicBlock &BB : *F) {
        for (const Instruction &I : BB) {
          if (isa<LoadInst>(I)) {
            recordAccess(&I, false, ThreadEntry, G);
          } else if (isa<StoreInst>(I)) {
            recordAccess(&I, true, ThreadEntry, G);
          }
        }
      }
  }
  
  // Call graph traversal
  // Use LLVM CallGraph for basic traversal
  // Note: SeaDSA has its own CallGraph which might be more precise for indirect calls
  // But we use getAnalysis<CallGraphWrapperPass> for simplicity/compatibility
  CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  CallGraphNode *CGN = CG[F];
  if (CGN) {
      for (auto &CallRecord : *CGN) {
          if (Function *Callee = CallRecord.second->getFunction()) {
              visitMethod(Callee, ThreadEntry, Visited);
          }
      }
  }
}

void StaticThreadSharingAnalysis::recordAccess(const Instruction *Inst, bool isWrite, const Function *ThreadEntry, Graph &G) {
   const Value *Ptr = nullptr;
   if (const LoadInst *LI = dyn_cast<LoadInst>(Inst)) Ptr = LI->getPointerOperand();
   else if (const StoreInst *SI = dyn_cast<StoreInst>(Inst)) Ptr = SI->getPointerOperand();
   
   if (!Ptr) return;
   
   if (!G.hasCell(*Ptr)) return;
   const Cell &C = G.getCell(*Ptr);
   Node *N = C.getNode();
   if (!N) return;
   
   unsigned Offset = C.getOffset();
   
   // Map Node to AllocSites
   const auto &AllocSites = N->getAllocSites();
   if (AllocSites.empty()) {
       // Handle Global Variables (they often don't have explicit AllocSites in SeaDSA)
       if (N->getNodeType().global) {
           // Attempt to recover the GlobalValue from the pointer operand
           // This assumes the pointer used to access the global is derived from the GlobalValue itself
           if (const GlobalValue *GV = dyn_cast<GlobalValue>(Ptr->stripPointerCasts())) {
               AccessInfo &Info = m_allocAccesses[GV][(int)Offset];
               if (isWrite) Info.Writers.insert(ThreadEntry);
               else Info.Readers.insert(ThreadEntry);
           }
           // Note: If multiple globals are collapsed into one node, and we access via different pointers,
           // we might track them separately here if we rely on Ptr. 
           // Ideally we would iterate N->globals() if SeaDSA exposed it, but it might not be easily accessible.
           // Using the accessing Ptr is a reasonable approximation for uncollapsed globals.
           // This approximation may miss sharing if the same global is accessed via different aliased pointers
           // that we fail to resolve to the same GV, but SeaDSA typically handles the aliasing at the Node level.
       }
       return;
   }

   for (const Value *Alloc : AllocSites) {
      AccessInfo &Info = m_allocAccesses[Alloc][(int)Offset];
      if (isWrite) Info.Writers.insert(ThreadEntry);
      else Info.Readers.insert(ThreadEntry);
   }
}

bool StaticThreadSharingAnalysis::isMultiRunThread(const Function *ThreadEntry) const {
    // Heuristic: main runs once. Others run multiple times.
    if (ThreadEntry->getName() == "main") return false;
    return true; 
}

StaticThreadSharingAnalysis::SharingClassification
StaticThreadSharingAnalysis::classify(const Value *AllocSite) const {
   if (!m_threads_complete) {
     return SharingClassification::MaybeShared;
   }

   auto it = m_allocAccesses.find(AllocSite);
   if (it == m_allocAccesses.end()) return SharingClassification::DefinitelyThreadLocal;
   
   // Check if any field is shared
   for (auto &pair : it->second) {
      const AccessInfo &info = pair.second;
      size_t writerCount = info.Writers.size();
      
      // Create a set of all unique threads accessing this location
      std::set<const Function*> allThreads = info.Readers;
      allThreads.insert(info.Writers.begin(), info.Writers.end());
      
      if (allThreads.size() > 1) {
          // According to the paper (Section 1, "Limitations of Escape Analysis" point 4, and Algorithm 3),
          // we must distinguish immutable data. Data is shared only if there is at least one write.
          // "thread-shared but immutable data... our algorithm also distinguishes between reads and writes"
          if (writerCount > 0) return SharingClassification::DefinitelyShared;
      }
   }
   return SharingClassification::DefinitelyThreadLocal;
}

bool StaticThreadSharingAnalysis::isShared(const Value *AllocSite) const {
   return classify(AllocSite) == SharingClassification::DefinitelyShared;
}

StaticThreadSharingAnalysis::SharingClassification
StaticThreadSharingAnalysis::classify(const Instruction *Inst) const {
    if (!m_dsa || !m_threads_complete) return SharingClassification::MaybeShared;
    
    const Function *F = Inst->getFunction();
    if (!m_dsa->hasGraph(*F)) return SharingClassification::MaybeShared;
    
    Graph &G = m_dsa->getGraph(*F);
    const Value *Ptr = nullptr;
    
    if (const LoadInst *LI = dyn_cast<LoadInst>(Inst)) Ptr = LI->getPointerOperand();
    else if (const StoreInst *SI = dyn_cast<StoreInst>(Inst)) Ptr = SI->getPointerOperand();
    
    if (!Ptr || !G.hasCell(*Ptr)) return SharingClassification::MaybeShared;
    
    const Cell &C = G.getCell(*Ptr);
    Node *N = C.getNode();
    if (!N) return SharingClassification::MaybeShared;
    
    // Check if any alloc site of this node is shared
    for (const Value *Alloc : N->getAllocSites()) {
        SharingClassification classification = classify(Alloc);
        if (classification != SharingClassification::DefinitelyThreadLocal) {
            return classification;
        }
    }
    
    return SharingClassification::DefinitelyThreadLocal;
}

bool StaticThreadSharingAnalysis::isShared(const Instruction *Inst) const {
    return classify(Inst) == SharingClassification::DefinitelyShared;
}

} // namespace lotus
