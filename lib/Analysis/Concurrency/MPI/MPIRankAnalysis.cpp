/**
 * @file MPIRankAnalysis.cpp
 * @brief Implementation of Symbolic MPI Rank Analysis
 */

#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace MPI;

bool RankExpr::mayEqual(const RankExpr &other) const {
  if (kind == Unknown || other.kind == Unknown) {
    return true; // Conservative
  }
  
  if (kind == Concrete && other.kind == Concrete) {
    return concrete_value == other.concrete_value;
  }
  
  if (kind == Concrete && other.kind == Range) {
    return concrete_value >= other.range_min && 
           concrete_value <= other.range_max;
  }
  
  if (kind == Range && other.kind == Concrete) {
    return other.concrete_value >= range_min && 
           other.concrete_value <= range_max;
  }
  
  if (kind == Range && other.kind == Range) {
    // Ranges overlap
    return !(range_max < other.range_min || other.range_max < range_min);
  }
  
  // Symbolic ranks may be equal
  return true;
}

bool RankExpr::mustEqual(const RankExpr &other) const {
  if (kind == Concrete && other.kind == Concrete) {
    return concrete_value == other.concrete_value;
  }
  
  // Cannot prove must-equality for symbolic/range/unknown
  return false;
}

MPIRankAnalysis::MPIRankAnalysis(Module &module) : m_module(module) {}

void MPIRankAnalysis::analyze() {
  errs() << "Starting MPI Rank Analysis...\n";
  
  identifyRankQueries();
  propagateRankInfo();
  analyzeRankBranches();
  
  errs() << "MPI Rank Analysis Complete!\n";
}

void MPIRankAnalysis::identifyRankQueries() {
  // Find all MPI_Comm_rank and MPI_Comm_size calls
  
  for (Function &func : m_module) {
    for (BasicBlock &bb : func) {
      for (Instruction &inst : bb) {
        if (CallInst *call = dyn_cast<CallInst>(&inst)) {
          Function *callee = call->getCalledFunction();
          if (!callee) continue;
          
          StringRef name = callee->getName();
          
          // MPI_Comm_rank(MPI_Comm comm, int *rank)
          if (name.equals("MPI_Comm_rank")) {
            if (call->arg_size() >= 2) {
              const Value *rank_ptr = call->getArgOperand(1);
              
              // Find stores to this pointer to track the rank value
              for (const Use &use : rank_ptr->uses()) {
                if (StoreInst *store = dyn_cast<StoreInst>(use.getUser())) {
                  if (store->getPointerOperand() == rank_ptr) {
                    // The stored value represents the symbolic rank
                    m_value_to_rank[store->getValueOperand()] = 
                        RankExpr::makeSymbolic();
                  }
                }
                else if (LoadInst *load = dyn_cast<LoadInst>(use.getUser())) {
                  if (load->getPointerOperand() == rank_ptr) {
                    // Load of rank value
                    m_value_to_rank[load] = RankExpr::makeSymbolic();
                  }
                }
              }
            }
          }
          
          // MPI_Comm_size(MPI_Comm comm, int *size)
          if (name.equals("MPI_Comm_size")) {
            if (call->arg_size() >= 2) {
              const Value *comm = call->getArgOperand(0);
              const Value *size_ptr = call->getArgOperand(1);
              
              // Track communicator size (would need more sophisticated analysis)
              // For now, assume MPI_COMM_WORLD
              m_comm_size[comm] = -1; // Unknown size
            }
          }
        }
      }
    }
  }
}

void MPIRankAnalysis::propagateRankInfo() {
  // Propagate rank expressions through the program
  // This is a simplified version; full implementation would use dataflow analysis
  
  for (Function &func : m_module) {
    for (BasicBlock &bb : func) {
      RankExpr current_rank = RankExpr::makeSymbolic();
      
      for (Instruction &inst : bb) {
        // Store current rank context
        m_inst_rank[&inst] = current_rank;
        
        // Update rank context based on control flow
        if (BranchInst *br = dyn_cast<BranchInst>(&inst)) {
          if (br->isConditional()) {
            // Check if branch condition depends on rank
            Value *cond = br->getCondition();
            
            if (ICmpInst *cmp = dyn_cast<ICmpInst>(cond)) {
              Value *op0 = cmp->getOperand(0);
              Value *op1 = cmp->getOperand(1);
              
              // Check if comparing rank with a constant
              auto it0 = m_value_to_rank.find(op0);
              auto it1 = m_value_to_rank.find(op1);
              
              if (it0 != m_value_to_rank.end() && isa<ConstantInt>(op1)) {
                // Rank comparison with constant
                int const_val = cast<ConstantInt>(op1)->getSExtValue();
                
                // Update rank on true/false branches
                // This would require more sophisticated path-sensitive analysis
              }
            }
          }
        }
      }
    }
  }
}

void MPIRankAnalysis::analyzeRankBranches() {
  // Analyze branches that depend on rank values
  // This enables precise analysis of rank-specific code paths
  
  // TODO: Implement path-sensitive rank analysis
  // For now, we conservatively assume all ranks may execute all code
}

RankExpr MPIRankAnalysis::getRankExpr(const Value *val) const {
  auto it = m_value_to_rank.find(val);
  if (it != m_value_to_rank.end()) {
    return it->second;
  }
  return RankExpr(); // Unknown
}

RankExpr MPIRankAnalysis::getRankAtInstruction(const Instruction *inst) const {
  auto it = m_inst_rank.find(inst);
  if (it != m_inst_rank.end()) {
    return it->second;
  }
  return RankExpr::makeSymbolic(); // Conservative: any rank
}

bool MPIRankAnalysis::sameRank(const Instruction *i1, 
                                const Instruction *i2) const {
  RankExpr rank1 = getRankAtInstruction(i1);
  RankExpr rank2 = getRankAtInstruction(i2);
  
  return rank1.mustEqual(rank2);
}

std::set<int> MPIRankAnalysis::getPossibleRanks(const Instruction *inst) const {
  std::set<int> ranks;
  
  RankExpr rank = getRankAtInstruction(inst);
  
  if (rank.kind == RankExpr::Concrete) {
    ranks.insert(rank.concrete_value);
  } else if (rank.kind == RankExpr::Range) {
    for (int r = rank.range_min; r <= rank.range_max; ++r) {
      ranks.insert(r);
    }
  } else {
    // Symbolic or Unknown: conservatively return empty set
    // Caller should interpret this as "any rank"
  }
  
  return ranks;
}
