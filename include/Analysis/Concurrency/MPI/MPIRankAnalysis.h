/**
 * @file MPIRankAnalysis.h
 * @brief Symbolic MPI Rank Analysis
 *
 * This file provides symbolic execution for MPI rank-dependent control flow.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#pragma once

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <map>
#include <set>

namespace MPI {

/**
 * @brief Symbolic rank expression
 */
class RankExpr {
public:
  enum Kind {
    Concrete,    ///< Concrete rank value (e.g., 0, 1, 2)
    Symbolic,    ///< Symbolic rank (from MPI_Comm_rank)
    Range,       ///< Range of ranks [min, max]
    Unknown      ///< Unknown rank
  };
  
  Kind kind;
  int concrete_value;  ///< For Concrete kind
  int range_min;       ///< For Range kind
  int range_max;       ///< For Range kind
  
  RankExpr() : kind(Unknown), concrete_value(-1), range_min(0), range_max(-1) {}
  
  static RankExpr makeConcrete(int rank) {
    RankExpr expr;
    expr.kind = Concrete;
    expr.concrete_value = rank;
    return expr;
  }
  
  static RankExpr makeSymbolic() {
    RankExpr expr;
    expr.kind = Symbolic;
    return expr;
  }
  
  static RankExpr makeRange(int min, int max) {
    RankExpr expr;
    expr.kind = Range;
    expr.range_min = min;
    expr.range_max = max;
    return expr;
  }
  
  bool mayEqual(const RankExpr &other) const;
  bool mustEqual(const RankExpr &other) const;
};

/**
 * @class MPIRankAnalysis
 * @brief Symbolic analysis of MPI rank values
 *
 * Tracks MPI rank values through the program to enable precise
 * analysis of rank-dependent control flow and communication patterns.
 */
class MPIRankAnalysis {
public:
  explicit MPIRankAnalysis(llvm::Module &module);
  
  /**
   * @brief Run the rank analysis
   */
  void analyze();
  
  /**
   * @brief Get the rank expression for a value
   */
  RankExpr getRankExpr(const llvm::Value *val) const;
  
  /**
   * @brief Get the rank at a specific instruction
   */
  RankExpr getRankAtInstruction(const llvm::Instruction *inst) const;
  
  /**
   * @brief Check if two instructions are in the same rank
   */
  bool sameRank(const llvm::Instruction *i1, const llvm::Instruction *i2) const;
  
  /**
   * @brief Get all ranks that may execute an instruction
   */
  std::set<int> getPossibleRanks(const llvm::Instruction *inst) const;

private:
  llvm::Module &m_module;
  
  // Mapping from value to rank expression
  std::map<const llvm::Value *, RankExpr> m_value_to_rank;
  
  // Rank at each instruction (context-sensitive)
  std::map<const llvm::Instruction *, RankExpr> m_inst_rank;
  
  // Size of communicators
  std::map<const llvm::Value *, int> m_comm_size;
  
  /**
   * @brief Identify MPI_Comm_rank calls
   */
  void identifyRankQueries();
  
  /**
   * @brief Propagate rank information through control flow
   */
  void propagateRankInfo();
  
  /**
   * @brief Analyze rank-dependent branches
   */
  void analyzeRankBranches();
};

} // namespace MPI
