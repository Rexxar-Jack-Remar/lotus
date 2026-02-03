/**
 * @file GlobalValueFlowAnalysis.h
 * @brief Global Value Flow Analysis using Dyck Value-Flow Graph
 *
 * This file provides a global value flow analysis that tracks data flow
 * from vulnerability sources to sinks using a Dyck-annotated value-flow graph.
 *
 * GVFA builds a fast reachability index first (for pruning), and lazily refines
 * to a precise engine when a query needs source attribution or an accurate
 * witness trace.
 *
 * Key Features:
 * - Forward and backward reachability queries
 * - CFL (Context-Free Language) reachability
 * - Context-sensitive analysis
 * - Path extraction for bug reporting
 * - Online queries (optional)
 *
 * @author Lotus Analysis Framework
 * @date 2025
 * @ingroup GVFA
 */

#ifndef ANALYSIS_GVFA_GLOBALVALUEFLOWANALYSIS_H
#define ANALYSIS_GVFA_GLOBALVALUEFLOWANALYSIS_H

#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckModRefAnalysis.h"
#include "Alias/DyckAA/DyckVFG.h"
#include "Analysis/GVFA/GVFAUtils.h"

#include <map>
#include <memory>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

// Forward declaration to avoid circular dependency
class GVFAVulnerabilityChecker;

namespace gvfa {
class GVFAEngine;
} // namespace gvfa

/**
 * @typedef ValueSitePairType
 * @brief Pair of a value and a site index for tracking data flow
 */
using ValueSitePairType = std::pair<const Value *, int>;

/**
 * @typedef VulnerabilitySourcesType
 * @brief Map of value-site pairs to source indices
 */
using VulnerabilitySourcesType = std::map<ValueSitePairType, int>;

/**
 * @typedef VulnerabilitySinksType
 * @brief Map of values to their sink values
 */
using VulnerabilitySinksType =
    std::map<const Value *, std::set<const Value *> *>;

/**
 * @class DyckGlobalValueFlowAnalysis
 * @brief Global Value Flow Analysis using Dyck VFG
 *
 * This analysis tracks data flow from vulnerability sources to sinks through
 * the value-flow graph. It supports various types of reachability queries
 * including standard reachability, CFL-reachability, and context-sensitive
 * reachability.
 *
 * The analysis uses a Dyck-annotated value-flow graph which encodes
 * pointer relations and context information, enabling precise tracking
 * of data flow across complex program structures.
 *
 * @note Uses DyckAliasAnalysis for pointer analysis
 * @see DyckVFG, DyckAliasAnalysis
 */
class DyckGlobalValueFlowAnalysis {
public:
  long AllQueryCounter = 0;      ///< Counter for all reachability queries
  long SuccsQueryCounter = 0;    ///< Counter for successor queries
  long SnapshotedOnlineTime = 0; ///< Time spent in online analysis

private:
  // Core components
  DyckVFG *VFG = nullptr;                ///< Pointer to the value-flow graph
  DyckAliasAnalysis *DyckAA = nullptr;   ///< Pointer to the alias analysis
  DyckModRefAnalysis *DyckMRA = nullptr; ///< Pointer to the mod-ref analysis
  Module *M = nullptr;                   ///< Pointer to the LLVM module

  // Sources and sinks
  VulnerabilitySourcesType Sources; ///< Map of vulnerability sources
  std::vector<std::pair<const Value *, int>> SourcesVec; ///< Vector of sources
  VulnerabilitySinksType Sinks; ///< Map of vulnerability sinks

  // Vulnerability checker
  std::unique_ptr<GVFAVulnerabilityChecker>
      VulnChecker; ///< Vulnerability checker instance

  // Analysis Engine
  std::unique_ptr<gvfa::GVFAEngine> Engine; ///< The GVFA engine for analysis

public:
  /**
   * @brief Construct a new Global Value Flow Analysis
   * @param M The LLVM module to analyze
   * @param VFG The value-flow graph to use
   * @param DyckAA The alias analysis to use
   * @param DyckMRA The mod-ref analysis to use
   */
  DyckGlobalValueFlowAnalysis(Module *M, DyckVFG *VFG,
                              DyckAliasAnalysis *DyckAA,
                              DyckModRefAnalysis *DyckMRA);

  /**
   * @brief Destroy the Global Value Flow Analysis
   */
  ~DyckGlobalValueFlowAnalysis();

  // Public interface
  /**
   * @brief Set the vulnerability checker
   * @param checker Unique pointer to the vulnerability checker
   */
  void
  setVulnerabilityChecker(std::unique_ptr<GVFAVulnerabilityChecker> checker);

  /**
   * @brief Run the analysis
   */
  void run();

  // Query interfaces
  /**
   * @brief Check forward reachability from a value
   * @param V The starting value
   * @param Mask Query mask
   * @return Result code indicating reachability status
   */
  int reachable(const Value *V, int Mask);

  /**
   * @brief Check backward reachability from a value
   * @param V The target value
   * @return true if the value is reachable from sources
   */
  bool backwardReachable(const Value *V);

  /**
   * @brief Check if a source can reach a specific value
   * @param V The target value
   * @param Src The source value to check
   * @return true if Src can reach V
   */
  bool srcReachable(const Value *V, const Value *Src) const;

  /**
   * @brief Check if any sink can reach a value backward
   * @param V The value to check
   * @return true if any sink can reach V
   */
  bool backwardReachableSink(const Value *V);

  /**
   * @brief Check if all sinks can reach a value backward
   * @param V The value to check
   * @return true if all sinks can reach V
   */
  bool backwardReachableAllSinks(const Value *V);

  // CFL reachability
  /**
   * @brief Check CFL reachability from one value to another
   * @param From The source value
   * @param To The target value
   * @return true if From can CFL-reach To
   */
  bool cflReachable(const Value *From, const Value *To) const;

  /**
   * @brief Check backward CFL reachability
   * @param From The source value
   * @param To The target value
   * @return true if From can backward CFL-reach To
   */
  bool cflBackwardReachable(const Value *From, const Value *To) const;

  /**
   * @brief Check context-sensitive reachability
   * @param From The source value
   * @param To The target value
   * @return true if From can context-sensitively reach To
   */
  bool contextSensitiveReachable(const Value *From, const Value *To) const;

  /**
   * @brief Check backward context-sensitive reachability
   * @param From The source value
   * @param To The target value
   * @return true if From can backward context-sensitively reach To
   */
  bool contextSensitiveBackwardReachable(const Value *From,
                                         const Value *To) const;

  // Path extraction for bug reporting
  /**
   * @brief Get the witness path from source to sink
   * @param From The source value
   * @param To The target value
   * @return Vector of values forming the path
   */
  std::vector<const Value *> getWitnessPath(const Value *From,
                                            const Value *To) const;

  // Utilities
  /**
   * @brief Print online query timing statistics
   * @param O The output stream
   * @param Title Optional title for the output
   */
  void printOnlineQueryTime(llvm::raw_ostream &O,
                            const char *Title = "[Online]") const;

  /**
   * @brief Get the vulnerability checker
   * @return Pointer to the vulnerability checker
   */
  GVFAVulnerabilityChecker *getVulnerabilityChecker() const {
    return VulnChecker.get();
  }

private:
  // Online reachability helpers
  /**
   * @brief Perform online reachability analysis
   * @param Target The target value
   * @return true if target is reachable
   */
  bool onlineReachability(const Value *Target);

  /**
   * @brief Perform online forward reachability
   * @param Node The starting node
   * @param visited Set of visited nodes
   * @return true if any target was reached
   */
  bool onlineForwardReachability(const Value *Node,
                                 std::unordered_set<const Value *> &visited);

  /**
   * @brief Perform online backward reachability
   * @param Node The current node
   * @param Target The target value
   * @param visited Set of visited nodes
   * @return true if target was reached
   */
  bool onlineBackwardReachability(const Value *Node, const Value *Target,
                                  std::unordered_set<const Value *> &visited);

  // CFL helpers
  /**
   * @brief Initialize the CFL analyzer
   */
  void initializeCFLAnalyzer();

  /**
   * @brief Perform a CFL reachability query
   * @param From The source value
   * @param To The target value
   * @param Forward Direction of analysis
   * @return true if reachable
   */
  bool performCFLReachabilityQuery(const Value *From, const Value *To,
                                   bool Forward) const;

  /**
   * @brief Internal CFL reachability query
   * @param From The source value
   * @param To The target value
   * @param Forward Direction of analysis
   * @return true if reachable
   */
  bool cflReachabilityQuery(const Value *From, const Value *To,
                            bool Forward) const;

  /**
   * @brief Get the node ID for a value
   * @param V The value
   * @return The node ID
   */
  int getValueNodeID(const Value *V) const;
};

#endif // ANALYSIS_GVFA_GLOBALVALUEFLOWANALYSIS_H
