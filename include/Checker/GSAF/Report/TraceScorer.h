#pragma once
#include "Checker/Framework/Subcommands.h"
#include "Checker/GSAF/API/Models.h"
#include "Checker/GSAF/Engine/Checker.h"
#include "Checker/GSAF/Support/GraphQueries.h"
#include "IR/GVFG/GuardedValueFlowTrace.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus::gsaf {
using namespace std;
using namespace llvm;
using namespace gvfg;

using namespace llvm;

class GSAFTraceScorer;

class GSAFClusteredTrace {

private:
  class Node {
    unordered_map<const GuardedValueFlowObject *, Node *>
        children; // Mapping values to  the children nodes
    const GuardedValueFlowObject *node_val;
    int level; // stores the level (i.e. the depth of the node from ROOT)
    bool is_key;
    bool is_dominated;
    void destroy();
    std::vector<GuardedValueFlowTrace *> source_traces;
    GuardedValueFlowTrace *key_trace;

  public:
    Node();
    virtual ~Node();

    void insert(GuardedValueFlowTrace &trace, int start_idx, bool dom);

    void dump(raw_ostream &O, int level, bool dump_all, DebugInfoAnalysis *DIA,
              GSAFTraceScorer *scorer);

    void get_all_children(std::unordered_set<Node *> &);
    void get_all_undomed_children(std::unordered_set<Node *> &);

    Node *get_child(GuardedValueFlowObject *child_inst);

    bool get_dom() { return is_dominated; }
    bool get_key() { return is_key; }
    const GuardedValueFlowObject *getGVFGNode() { return node_val; }
    int get_num_child() { return children.size(); }
    int get_level() { return level; }

    static const int ROOT = -1;
    static const int LEAF = -2;
  };

  Node root;

public:
  // insert the trace into the clustered trace,
  //  dom indacate whether the trace is dominated by other traces
  void insert(GuardedValueFlowTrace &trace, bool dom);

  // dump the clustered trace to ostream O and applies the DBG info DIA
  //  dump_all=true means all the reports are dumped
  //                       false means only the reports that are valid are
  //                       dumped
  void dump(raw_ostream &O, bool dump_all, DebugInfoAnalysis *DIA,
            GSAFTraceScorer *scorer);

  // Return the number of clusters in the clustered trace
  int get_num_clusters();
};

class GSAFTraceScorer {
  // Trace scorer takes a set of GuardedValueFlowTraces (Better for a
  // same/similar bug type reported by a same/similar reporter)
  //       as input, and give a score to each of the traces
  //
  // We accept any kind of GVFG trace as input, say any sequence of
  // GuardedValueFlowObjects(including nullptr's) is allowed
  //
  // Especially, to indicate a value flow, please add the child and parent node
  // into the GVFG trace
  //          in the order child->parent, with no other nodes in between, and
  //          the scorer shall take the pattern as a value flow
  // To indicate a choice of a PHI-gated function,
  //          please just put the choice node and the phi-node in order in the
  //          trace with no other GuardedValueFlowObject in between, and the
  //          phi-gated function shall be considered
  // To indicate a caller->callee value flow,
  //          just put the corresponding GVFG call site node in the trace
  //          followed by any node in the callee
  // To indicate a callee->caller value flow,
  //          please put the corresponding GVFG return site node in the trace
  //          followed by a common/pseudo GVFG output or the operand node whose
  //          LLVM value is the call-site instruction in the caller side
  //

public:
  typedef uint64_t tactic_t;

private:
  struct ReportItem {
    GuardedValueFlowTrace *trace;

    // TODO: Add tags here
    bool is_dominated;
    bool is_valid;
    float confidence;
    float trace_confidence;
    float path_confidence;
    float context_confidence;

    float compute_confidence();
    int compute_score();
  };

  PackedTypeLayout *DL = nullptr;
  DebugInfoAnalysis *DIA = nullptr;
  GSAFChecker *CG = nullptr;
  GSAFChecker *dom_pass = nullptr;
  GSAFModels *memory_spec = nullptr;
  GSAFModels *IO_spec = nullptr;
  GSAFModels *taint_spec = nullptr;

  tactic_t tactic;

  //------- Bug report storage-------
  std::vector<ReportItem> report_items;

  // mapping reports to indices
  unordered_map<GuardedValueFlowTrace *, int> report_index;

  // cache the non-null ptrs
  unordered_map<Function *, unordered_set<Value *>> non_null_cache;

  //------- General Util functions for GSAFTraceScorer-------
  // return the index of an instruction in report_items,
  // GSAFTraceScorer::UNKNOWN is returned if the instruction is not in
  // report_items
  int get_index(GuardedValueFlowTrace *);

  static const int UNKNOWN = -1;

  //------- Domination checking utils-------
  GSAFClusteredTrace clustered_trace;

private:
  // return true if the report with index idx in report_items is dominated by
  // other reports idx can be any value, if idx is invalid such as -10, we
  // return false
  bool is_dominated(int idx);

  //  return true if the report with index idx in report_items is valid
  // idx can be any value, if idx is invalid such as -10, we return true
  bool is_valid(int idx);

  // return the confidence of the given trace
  // idx can be any value, if idx is invalid such as -10, we return 1.0f
  float get_confidence(int idx);

  // return the clustered trace
  GSAFClusteredTrace &get_clustered_trace();

  //-------Scoring Tactics Impl-------
  // TODO : Add scoring tactic impls here.

  // check for each trace and
  //   (1) store  if the trace is dominated by other traces into the
  //   "is_dominated" tag in the report_items (2) generate a clustered trace
  //   stored in clustered_trace
  void dominate_checking();

  // check for the path validity for each trace
  void path_validity_checking();

  // compute confidence
  void trace_confidence_checking();
  void path_confidence_checking();
  void context_confidence_checking();

  // Process dominate checking
  void do_dominate_checking_on_value();
  void do_dominate_checking_on_gvfg_node();

public:
  class Iterator {
  public:
    enum iter_type { ALL_VALID, NON_DOM_ONLY };

  private:
    GSAFTraceScorer &parent;
    int index;
    enum iter_type type;

  public:
    Iterator(GSAFTraceScorer &pv);
    Iterator(int idx, iter_type t, GSAFTraceScorer &pv);
    Iterator(const Iterator &iter);
    void operator++(int);
    bool operator==(const Iterator &iter);
    bool operator!=(const Iterator &iter);
    GuardedValueFlowTrace &operator*();
    GuardedValueFlowTrace *operator->();
  };

public:
  // We use a 64 bit bit-vector to denote the tactics (strategy) to track in a
  // profiler All tactic flag definitions are enclosed in TACTIC_FLAGS struct
  struct TACTIC_FLAGS {

    /// Tactic notes:
    /// Users can customize tactics for better scoring result
    ///     For example, for path sensitive analysis, there is no need to
    ///     perform path validity checking (PATH or VALIDITY)
    ///
    /// Detailed tactics are as follows : (can be combined using bit-wise-or
    /// "|", such as TRACE|DOMINATION|CONTEXT|NO_CALLEE_CONTEXT) Confidence :
    /// TRACE/PATH/CONTEXT
    ///         TRACE      : Running trace value-flow confidence checking
    ///         CONTEXT    : Running context heuristic to check whether the
    ///         trace is affected by unanalyzed functions
    ///                     (Considering values/conditions using values from
    ///                     caller/callees, no value passing to callers/callees
    ///                     if used without sub commands)
    ///               CONTEXT_WITH_VALUE_SINK : Considering effect of trace
    ///               value passed to callers/callees
    ///                                         (only considers side-effects if
    ///                                         used without sub-commands)
    ///                     CONTEXT_VALUE_SINK_CONSIDER_DIRECT : Considering
    ///                     both side-effects and direct value passing to
    ///                     callers/callees CONTEXT_VALUE_SINK_SUMMARY_ONLY    :
    ///                     Considering only side-effects on summary nodes that
    ///                     are not accurately handled
    ///               NO_CALLER_CONTEXT       : Considering no effect from
    ///               caller NO_CALLEE_CONTEXT       : Considering no effect
    ///               from callee ( similar effect to
    ///               NO_CALLEE_COND|NO_CALLEE_VAL) NO_CALLEE_COND          :
    ///               Considering no effect from callee for condition checking
    ///               NO_CALLEE_VAL           : Considering no effect from
    ///               callee on the trace values. NO_CALLER_COND          :
    ///               Considering no effect from caller for condition checking
    ///               NO_CALLER_VAL           : Considering no effect from
    ///               caller on the trace values. CONTEXT_IGNORE_LIBRARY  :
    ///               Considering no effect from library function calls
    ///                                         Even Maring NO_CALLEE_CONTEXT,
    ///                                         the effect of library function
    ///                                         calls are still considered
    ///                                         Please actively mark this flag
    ///                                         to ignore library effects if
    ///                                         required
    ///               CONTEXT_IGNORE_FUNCTION_POINTER : Ignore the effect of
    ///               possible error on function pointer resolution
    ///         PATH       : Running path validity heuristic according to the
    ///         number of paths
    ///
    /// Validity   : VALIDITY
    ///         VALIDITY   : Running path validity checking using SMT solving
    ///
    /// Domination : DOMINATION
    ///         DOMINATION : Running domination checking to lower the scores for
    ///         similar reports to checked ones
    ///

    // Running trace value-flow confidence checking
    static const tactic_t TRACE = 0x1;
    // Running path validity checking using SMT solving
    static const tactic_t VALIDITY = 0x2;
    // Running domination checking
    static const tactic_t DOMINATION = 0x4;
    // Running path validity heuristic according to the number of paths
    // Usually, when SMT based path validity checking (VALIDITY) is enabled, we
    // do not need to activate path validity heuristic
    static const tactic_t PATH = 0x8;
    // Running context heuristic to check whether the trace is affected by
    // unanalyzed functions
    static const tactic_t CONTEXT = 0x10;
    // Sub-command to CONTEXT : Considering no effect from caller. Used with
    // CONTEXT
    static const tactic_t NO_CALLER_CONTEXT = 0x20;
    // Sub-command to CONTEXT : Considering no effect from callee. Used with
    // CONTEXT
    static const tactic_t NO_CALLEE_CONTEXT = 0x40;
    // Sub-command to CONTEXT : Considering no effect from callee for condition
    // checking. Used with CONTEXT
    static const tactic_t NO_CALLEE_COND = 0x80;
    // Sub-command to CONTEXT : Considering no effect from callee on the trace
    // values. Used with CONTEXT
    static const tactic_t NO_CALLEE_VAL = 0x100;
    // Sub-command to CONTEXT : Considering effect of trace value passed to
    // caller/callee. Used with CONTEXT Only considering side-effects, but no
    // direct value passing
    static const tactic_t CONTEXT_WITH_VALUE_SINK = 0x200;
    // Sub-command to CONTEXT/CONTEXT_WITH_VALUE_SINK :
    // Considering both side-effects and direct value passing for effect of
    // trace value passed to caller/callee. Used with CONTEXT &&
    // CONTEXT_WITH_VALUE_SINK
    static const tactic_t CONTEXT_VALUE_SINK_CONSIDER_DIRECT = 0x400;
    // Sub-command to CONTEXT/CONTEXT_WITH_VALUE_SINK :
    // Considering only side-effects on summary nodes that are not accurately
    // handled
    //             for effect of trace value passed to caller/callee.
    // Used with CONTEXT && CONTEXT_WITH_VALUE_SINK
    static const tactic_t CONTEXT_VALUE_SINK_SUMMARY_ONLY = 0x800;
    // Sub-command to CONTEXT : Ignore the effect of library function calls
    // usually a checker shall not consider library function calls
    // If a checker shall not consider library function calls, library function
    // calls shall not be ignored Otherwise, if the checker already considers
    // library function calls, please mark this flag to 1 Used with CONTEXT
    static const tactic_t CONTEXT_IGNORE_LIBRARY = 0x1000;
    // Sub-command to CONTEXT : Ignore the effect of errors on the function
    // pointer resolution Used with CONTEXT
    static const tactic_t CONTEXT_IGNORE_FUNCTION_POINTER = 0x2000;
    // Sub-command to CONTEXT : Considering no effect from caller for condition
    // checking. Used with CONTEXT
    static const tactic_t NO_CALLER_COND = 0x4000;
    // Sub-command to CONTEXT : Considering no effect from caller on the trace
    // values. Used with CONTEXT
    static const tactic_t NO_CALLER_VAL = 0x8000;

    // promoted tactics for some representative checkers and meaningful tactic
    // compositions
    static const tactic_t
        GSAF_WITH_SYMBOLIC_SUMMARY = // Proposed scoring tactic for GSAF based
                                     // NPD-like checkers inlining symbolic
                                     // summaries during the execution of the
                                     // checker
        TRACE | CONTEXT | NO_CALLEE_CONTEXT;
    static const tactic_t
        GSAF_WITHOUT_SYMBOLIC_SUMMARY = // Proposed scoring tactic for GSAF
                                        // based NPD-like checkers WITHOUT
                                        // inlining symbolic summaries during
                                        // the execution of the checker
        TRACE | CONTEXT | NO_CALLEE_VAL;
    static const tactic_t
        GSAF_ML_WITH_SYMBOLIC_SUMMARY = // Proposed scoring tactic for GSAF
                                        // based ML-like checkers inlining
                                        // symbolic summaries during the
                                        // execution of the checker
        TRACE | CONTEXT | NO_CALLEE_CONTEXT | CONTEXT_WITH_VALUE_SINK |
        CONTEXT_VALUE_SINK_SUMMARY_ONLY;
    static const tactic_t
        GSAF_ML_WITHOUT_SYMBOLIC_SUMMARY = // Proposed scoring tactic for GSAF
                                           // based ML-like checkers WITHOUT
                                           // inlining symbolic summaries during
                                           // the execution of the checker
        TRACE | CONTEXT | NO_CALLEE_VAL | CONTEXT_WITH_VALUE_SINK |
        CONTEXT_VALUE_SINK_SUMMARY_ONLY;
    static const tactic_t
        SSU_CHECKER_NPD = // Proposed scoring tactic for SSU based NPD-like
                          // checkers (Enabling SMT solving)
        TRACE | VALIDITY | CONTEXT | NO_CALLEE_VAL;
    static const tactic_t
        SSU_CHECKER_NPD_LIGHT_WEIGHT = // Proposed light-weighted scoring tactic
                                       // for SSU based NPD-like checkers (No
                                       // SMT solving)
        TRACE | PATH | CONTEXT | NO_CALLEE_VAL;
    static const tactic_t
        SSU_CHECKER_ML = // Proposed scoring tactic for SSU based ML-like
                         // checkers (Enabling SMT solving)
        TRACE | VALIDITY | CONTEXT | NO_CALLEE_VAL | CONTEXT_WITH_VALUE_SINK;
    static const tactic_t
        SSU_CHECKER_ML_LIGHT_WEIGHT = // Proposed light-weighted scoring tactic
                                      // for SSU based ML-like checkers (No SMT
                                      // solving)
        TRACE | PATH | CONTEXT | NO_CALLEE_VAL | CONTEXT_WITH_VALUE_SINK;
    static const tactic_t CONFIDENCE_ALL = // Enabling All confidence checking
        TRACE | PATH | CONTEXT;
    static const tactic_t CONTEXT_COND_ONLY = // Considering only control deps
                                              // in context confidence checking
        CONTEXT | NO_CALLEE_VAL | NO_CALLER_VAL;
    static const tactic_t
        TACTIC_ALL = // For testing, all tactics (including sub-tactics) in
                     // scoring system shall be enabled
        TRACE | PATH | CONTEXT | CONTEXT_WITH_VALUE_SINK |
        CONTEXT_VALUE_SINK_CONSIDER_DIRECT | VALIDITY | DOMINATION;
    static const tactic_t TACTIC_NOTHING = 0x0;
  };

public:
  //------- General Interfaces -------
  // See tactic notes before to customize tactics
  GSAFTraceScorer(std::vector<GuardedValueFlowTrace *> &reports,
                  PackedTypeLayout *DL = nullptr,
                  DebugInfoAnalysis *DIA = nullptr, GSAFChecker *CG = nullptr,
                  GSAFChecker *dom_tree_pass = nullptr,
                  GSAFModels *memory_spec = nullptr,
                  GSAFModels *IO_spec = nullptr,
                  GSAFModels *taint_spec = nullptr,
                  tactic_t tactic = TACTIC_FLAGS::TACTIC_NOTHING);

  virtual ~GSAFTraceScorer();

  // Execute the scoring strategies
  //  All the scoring strategies should be filled in to check_all function
  void check_all();

  //------- Interfaces for path validity checker -------
  // return true if the trace is valid;
  bool is_valid(GuardedValueFlowTrace *trace);

  // return true if the input trace is dominated by other traces
  bool is_dominated(GuardedValueFlowTrace *trace);

  // return the confidence of the given trace
  float get_confidence(GuardedValueFlowTrace *trace);

  // Dump all the reports in a clustered format to ostream O and applies the DBG
  // info DIA
  void dump_clustered_trace(raw_ostream &O);

  // Dump only the reports that are not dominated by other traces
  //                                in a clustered format to ostream O and
  //                                applies the DBG info DIA
  void dump_clustered_trace_undomed(raw_ostream &O);

  // Iterating the traces
  // all_begin/end, iterating all valid traces
  // nondom_begin/end iterating only non-dominated traces
  Iterator all_begin();
  Iterator all_end();
  Iterator nondom_begin();
  Iterator nondom_end();

  // Dump the statistical information of scoring system
  void dump_statistical_info(raw_ostream &O);

  friend class GSAFClusteredTrace;

private:
  void process_step_for_context(
      std::vector<const GuardedValueFlowNode *> &worklist,
      unordered_set<Function *> &func_in_trace_without_caller,
      unordered_set<Function *> &func_in_trace,
      unordered_set<const GuardedValueFlowObject *> &processed_nodes,
      unordered_set<const GuardedValueFlowNode *> *callee_arg_dep,
      std::vector<GuardedValueFlowRegionNode *> path_choices,
      float &caller_confidence, float &callee_confidence,
      bool is_caller_confidence_enabled, bool is_callee_confidence_enabled,
      bool is_library_considered, float caller_unit_confidence,
      float callee_unit_confidence, bool is_ignore_phi);
};

} // namespace lotus::gsaf
