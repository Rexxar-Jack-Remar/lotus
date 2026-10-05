#include "Checker/GSAF/Report/TraceScorer.h"

#include "Checker/Framework/Subcommands.h"
#include "Checker/GSAF/API/Models.h"
#include "Checker/GSAF/Engine/Checker.h"
#include "Checker/GSAF/Support/CheckerServices.h"
#include "Checker/GSAF/Support/GraphQueries.h"
#include "IR/GVFG/GuardedValueFlowTrace.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/User.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus::gsaf {
using namespace std;
using namespace llvm;
using namespace gvfg;

#undef DEBUG_TYPE
#define DEBUG_TYPE "GSAFTraceScorer"

using namespace llvm;
using namespace std;

static cl::opt<bool>
    scorer_dump_score("gsaf.scorer-dump-score",
                      cl::sub(lotus::checker::tooling::gsafSubCommand()),
                      cl::desc("dump scores computed in scorer"),
                      cl::init(false), cl::ReallyHidden);

static cl::opt<bool> scorer_dump_path_confidence(
    "gsaf.scorer-dump-path-confidence",
    cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc("dump path confidence details computed by trace scorer"),
    cl::init(false), cl::ReallyHidden);

static cl::opt<bool> scorer_dump_context_confidence(
    "gsaf.scorer-dump-context-confidence",
    cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc("dump context confidence details computed by trace scorer"),
    cl::init(false), cl::ReallyHidden);

static cl::opt<bool> scorer_dump_clustered_trace(
    "gsaf.scorer-dump-clustered-trace",
    cl::sub(lotus::checker::tooling::gsafSubCommand()),
    cl::desc("dump the traces in the trace scorer in a tree format"),
    cl::init(false), cl::ReallyHidden);

// ========================================README==================================

/*
 * This file is the core implementation of the scoring system
 *
 * For each bug report, we give a 0-100 int score indicating the importance of
 * the bug report The higher score, the more important of the bug report
 *
 * Score 100 means that the bug report is very import on that
 * (1) It is very likely to be a true bug
 * (2) The bug type is critical
 *
 * We give score 0 to bugs that meets either of the follows:
 * (1) Trace is proved invalid
 * (2) The triggered bug has importance 0
 *
 * We give score 1 to bugs that meet either of the follows:
 * (1) We have very low confidence that the bug is a true positive
 *
 * The score is computed as (Importance*sqrt(confidence))
 *  where sqrt is specified by var CONFIDENCE_IMPORTANCE in function
 * score_with_confidence()
 *
 * The scoring system contains 5 parts: constructive confidence, bug type
 * importance, general confidence, validity, and domination info Constructive
 * Confidence: Specified by checker writers, computed during the execution of
 * checkers. This gives checkers the ability to control the scoring according to
 * the special trace constructing inaccuracy. The value should be a float
 * between 0-1 specifying the probability that the report is a true positive
 * according to the trace constructing procedure General factors that affect the
 * report validity need not be considered here Default is 1.0 (No special
 * constructing inaccuracy specified) In this file, this value is passed as an
 * external parameter, and we use it as probability Bug Type Importance: The
 * importance of the bug according to its type. For example exploitable bugs may
 * have high importance, while bad programming practice shall have low
 * importance. The value shall be [0,100] as an integer, which shall be the
 * upper bound of the score for reports of such type Default is 80 (important,
 * but not critical) In this file, this value is passed as an external
 * parameter, and we use it as the upper bound of the report score General
 * Confidence: The confidence that the bug report is a true positive. We assume
 * that if the trace path is feasible and the bug trace does not depend on
 * external values, the trace represent a true bug with the probability
 * specified by "constructive confidence". Each scoring factor is considered
 * ONLY ONCE in the general confidence computation. General confidence is a
 * [0-1] float value indicating a probability that the trace path is feasible
 * and the bug trace does not depend on external values, which considers the
 * value flow confidence, path validity, the path correlation, and the external
 * value from caller/callee(including libraries) of the trace Users can define
 * tactics to customize the general confidence computation. For example, path
 * sensitive analysis does not require path validity checking, but only requires
 * external dependency checking See Report/Scorer/GSAFTraceScorer.h for details
 * of the tactics. The checking procedure is implemented in this file. Validity:
 *       Using SMT solver to eliminate infeasible reports.
 *       boolean value:
 *          If true, score is not affected.
 *          If false, score is 0.
 *  Domination:
 *       Report correlation checking among reports of a same type.
 *       If the triggering of a indicates the triggering of b, then a is
 * dominated by b
 *
 * The following "Basic setting for scoring system" section specifies the
 * general settings.
 *  *_CONFIDENCE means the probability that * may occur
 *  GROUP_CORRELATION means that the correlation of scoring factors in a same
 * group, where a group is the set of variable reachable to a same source, e.g.
 * [a, a->f, a->g, a->f->h, a->g.c ...] If the group correlation is 0, we treat
 * each affecting factor as an independent event, If the group correlation is 1,
 * we treat all affecting factors in a same group as one single same event
 *    	 Especially, we always treat the effect of variables in different groups
 * as independent events PATH_PATH_CORRELATION_PERCENTAGE, is the percentage of
 * path infeasible caused by path-path correlation. In general, we have
 * path-path correlation and path-value correlation. path-path correlation is
 * that if(a>0) and if(a<-2) can never be executed both. path-value correlation
 * is that if(a>0)b=2; and if(b<0) cannot be executed both
 *       PATH_PATH_CORRELATION_PERCENTAGE is used to eliminate the scoring
 * factor that an external variable can seldom affect a trace validity if there
 * are few path correlation opportunities IS_STEP_GRANULARITY, is a bool value
 * that specify if for each step of the reported trace, each type of scoring
 * factors is considered only once. Say we only consider whether a step is
 * affected by a type of factor, but not how large is the affection Especially,
 * when IS_STEP_GRANULARITY is true, we force GROUP_CORRELATION = 1.0
 *  DOMINATION_FACTOR is a [0-1] float value identify how much consideration of
 * the report correlation issue we consider for the scoring system Say, we only
 * give one of a group of similar reports a high score
 *
 */

// =========================Basic setting for scoring
// system=======================

#define CONFIDENCE_OUTPUT_FORMAT "%.3f"

// Confidence Settings, value ranges in [0,1], 0 means no confidence and 1 means
// full confidence Confidence for each path condition to be feasible
static const float PATH_FEASIBLE_CONFIDENCE = 0.8f;
// Confidence that two basic blocks, with no domination relation, has path
// correlation
static const float PATH_CORRELATION_CONFIDENCE = 0.1f;
// Confidence that the trace value from an unanalyzed caller shall affect the
// trace validity
static const float CALLER_VAL_EFFECT_CONFIDENCE = 0.3f;
// Confidence that the trace value from an unanalyzed callee shall affect the
// trace validity
static const float CALLEE_VAL_EFFECT_CONFIDENCE = 0.5f;
// Confidence that the value from an unanalyzed caller in the path condition
// shall affect the trace validity
static const float CALLER_COND_EFFECT_CONFIDENCE = 0.3f;
// Confidence that the value from an unanalyzed callee in the path condition
// shall affect the trace validity
static const float CALLEE_COND_EFFECT_CONFIDENCE = 0.3f;
// Confidence that the trace value passed to an unanalyzed caller shall affect
// the trace validity
static const float CALLER_SINK_EFFECT_CONFIDENCE = 0.7f;
// Confidence that the trace value passed to an unanalyzed callee shall affect
// the trace validity
static const float CALLEE_SINK_EFFECT_CONFIDENCE = 0.7f;
// Confidence that the value, either trace value or path condition passed to an
// unanalyzed caller shall affect the trace validity
static const float CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR = 0.5f;
// Confidence that value in an GVFG summary node is really passed to the
// caller/callee
static const float SUMMARY_CONFIDENCE = 0.5f;
// Confidence that the function pointer is correctly resolved
static const float FUNCTION_POINTER_CORRECT_CONFIDENCE = 0.8f;

// Effect correlation for factors of same group.
// float ranges between 0.0f and 1.0f
// 0.0f means that the factors of same group are treated as independent events
// 1.0f means that either all the factors in a same group affect the trace
// validity
//                  or all factors in the same group has no effect
//
// Group of factors is a set of factors that may have correlation to each other,
//             e.g. the arguments reachable to a same source
static const float GROUP_CORRELATION = 0.95f;

// the percentage of correlation caused by path-path correlation
// here the value is [0-1]. E,g, 0.8f means 80%
static const float PATH_PATH_CORRELATION_PERCENTAGE = 0.8f;

// True if for each trace step, we consider each scoring factor only once
bool IS_STEP_GRANULARITY = true;

// Importance of the dominated trace
// Value range from [0,1]
// 0.0 means dominated trace is not cared at all,
// 1.0 means dominated traces has same importance as un-dominated traces
static const float DOMINATION_FACTOR = 1.0f;

static int score_with_confidence(int score, float confidence) {
  // Basic settings:
  // In general the meaning of the scores are as follows:
  //  0       : proven invalid, should not be reported
  //  1       : not proven invalid, yet with extremely low confidence, should
  //  not be reported 2       : not proven invalid, yet with relatively low
  //  confidence, can be reported but the risk is high 3 - 100 : normally
  //  computed score, much higher confidence to the reports with score 1 or 2
  // Higher score means higher confidence/importance of the report

  // A very small positive number, below which we regard the confidence as 0
  const float EPSILON = 0.000001f;

  // A float Number identifying the importance of confidence to trace during
  // ranking Value can be ANY float. Near negative infinite means that we can
  // tolerate some small confidence traces in the bug report 0.0 means a neutral
  // setting Near positive infinite means that we tolerate no traces with small
  // confidence level.
  const float CONFIDENCE_IMPORTANCE = -1.0f;

  if (score == 0) {
    // A trace with score 0 will not be reported, without considering the
    // confidence
    return 0;
  }

  if (confidence < EPSILON) {
    // Very small confidence, the trace should not be reported
    // We return 1 to indicate that the trace is not proved invalid but the
    // confidence is extremely low
    return 1;
  }

  float normalized_confidence = pow(confidence, pow(2, CONFIDENCE_IMPORTANCE));

  int result = static_cast<int>(static_cast<float>(score) * normalized_confidence);

  // Adjust result
  // Distinguishing (1) invalid traces and traces with very low scores
  // and (2) traces with very low scores and traces with relatively low scores
  if (result == 0) {
    // computed score 0 means that the score is relatively low, yet not
    // extremely low, we give a score 2
    result = 2;
  } else if (result <= 2) {
    // report with computed score 1,2 or 3 are normalized to 3
    result = 3;
  }

  return result;
}

// ======================= GSAFClusteredTraceNode class
// implementation==========================

GSAFClusteredTrace::Node::Node() {
  children.clear();
  level = ROOT;
  node_val = nullptr;
  is_key = false;
  is_dominated = true;
  key_trace = nullptr;
}

GSAFClusteredTrace::Node::~Node() { destroy(); }

void GSAFClusteredTrace::Node::insert(GuardedValueFlowTrace &trace,
                                      int start_idx, bool dom) {
  int valid_start_idx = trace.getValidStartIdx();
  int key_idx = trace.getKeyIdx();

  if (key_idx == GuardedValueFlowTrace::IDX_INVALID ||
      valid_start_idx == GuardedValueFlowTrace::IDX_INVALID) {
    // Invalid trace
    return;
  }

  assert(((start_idx >= 0 && start_idx <= key_idx) || (start_idx == ROOT)) &&
         "Incorrect Trace Index");

  level = start_idx;
  source_traces.push_back(&trace);

  if (level != ROOT) {
    const GuardedValueFlowObject *current_value = trace[start_idx];
    is_key = (start_idx == key_idx) ? true : false;
    if (start_idx == key_idx)
      key_trace = &trace;
    if (node_val == nullptr) {
      node_val = current_value;
    }

    assert(node_val == current_value &&
           "Value inserted into incorrect trace node");

    // the node is marked as dominated if all the traces that have this node are
    // dominated by other traces
    is_dominated = dom && is_dominated;
  } else {
    // Root is never dominated
    is_dominated = false;
  }

  if (start_idx != key_idx) {
    // The node is not a leaf
    const GuardedValueFlowObject *next_value;
    int next_idx;
    if (start_idx == ROOT) {
      next_idx = valid_start_idx;
      next_value = trace[next_idx];
    } else {
      next_idx = start_idx + 1;
      next_value = trace[next_idx];
    }

    GSAFClusteredTrace::Node *child;

    auto Res = children.find(next_value);
    if (Res == children.end()) {
      // child does not exist
      child = new GSAFClusteredTrace::Node;
      children[next_value] = child;
    } else {
      child = Res->second;
    }
    child->insert(trace, next_idx, dom);
  }
}

void GSAFClusteredTrace::Node::dump(raw_ostream &O, int start_level,
                                    bool dump_all, DebugInfoAnalysis *DIA,
                                    GSAFTraceScorer *scorer) {
  if (get_dom() == true && dump_all == false) {
    // ROOT is never dominated
    return;
  }

  for (int i = 0; i < start_level; i++) {
    O << "|--";
  }

  if (level == ROOT) {
    O << "ROOT\n";
  } else {
    string dom, info;
    dom = is_dominated ? "(domed)" : "";

    O << dom << " ";

    O << "(" << node_val << ")"
      << " ";

    if (node_val) {
      if (Instruction *inst = node_val->getDebugInstruction()) {
        O << "[" << DIA->getSourceLocation(inst) << "]";
      } else {
        if (const GuardedValueFlowNode *seg_node =
                dyn_cast<GuardedValueFlowNode>(node_val)) {
          O << seg_node->getDescription();
        } else {
          O << "**" << *node_val << "**";
        }
      }
    } else {
      O << "**<NA>**";
    }

    if (is_key) {
      if (key_trace) {
        bool valid = scorer->is_valid(key_trace);
        if (valid)
          O << "<Valid>";
        else
          O << "<Invalid>";

        O << "[SCORE = " << key_trace->get_score() << "]";
      }
      O << " "
        << "(Vulnerable Point)";
    }
    O << "\n";
  }

  LLVM_DEBUG(if (node_val != nullptr) {
    for (int i = 0; i < start_level; i++) {
      O << "|--";
    }
    O << node_val << "\n";
    for (int i = 0; i < start_level; i++) {
      O << "|--";
    }
    O << "\n";
  });

  for (auto Res : children) {
    GSAFClusteredTrace::Node *child = Res.second;
    if (child->get_dom() == false || dump_all == true) {
      child->dump(O, start_level + 1, dump_all, DIA, scorer);
    }
  }
}

void GSAFClusteredTrace::Node::get_all_children(unordered_set<Node *> &res) {
  res.clear();
  for (auto Res : children) {
    Node *child = Res.second;
    res.insert(child);
  }
}

void GSAFClusteredTrace::Node::get_all_undomed_children(
    unordered_set<Node *> &res) {
  res.clear();
  for (auto Res : children) {
    Node *child = Res.second;
    if (child->get_dom() == false) {
      res.insert(child);
    }
  }
}

GSAFClusteredTrace::Node *
GSAFClusteredTrace::Node::get_child(GuardedValueFlowObject *child_inst) {
  if (children.find(child_inst) == children.end()) {
    // child_inst does not exist;
    return nullptr;
  }
  return children[child_inst];
}

void GSAFClusteredTrace::Node::destroy() {
  for (auto Res : children) {
    Node *child = Res.second;
    delete child;
  }
  children.clear();
}

// ======================= GSAFClusteredTrace class
// implementation==========================

// dump_all means if we dump all reports
void GSAFClusteredTrace::dump(raw_ostream &O, bool dump_all,
                              DebugInfoAnalysis *DIA, GSAFTraceScorer *scorer) {
  root.dump(O, 0, dump_all, DIA, scorer);
}

// the whole trace is Trace[length-1]->Trace[length-2]->...->Trace[0]->key_val,
//            dom means if the trace is dominated by other traces
void GSAFClusteredTrace::insert(GuardedValueFlowTrace &trace, bool dom) {
  // Value* key_val = trace.get_key();
  root.insert(trace, Node::ROOT, dom);
}

int GSAFClusteredTrace::get_num_clusters() { return root.get_num_child(); }

// ======================= GSAFTraceScorer class
// implementation==========================

float GSAFTraceScorer::ReportItem::compute_confidence() {
  float constructive_confidence =
      trace != nullptr ? trace->get_constructive_confidence() : 1.0f;

  confidence = trace_confidence * path_confidence * context_confidence *
               constructive_confidence;
  return confidence;
}

int GSAFTraceScorer::ReportItem::compute_score() {
  if (!is_valid) {
    // Invalid trace have lowest score, which should never be reported
    return 0;
  }

  if (!trace) {
    // Trace does not exist, we give lowest score, which should never be
    // reported
    return 0;
  }

  // BugTypeImportance is used as the highest score for the given trace
  // which means that even if we have full confidence that the trace targets on
  // a true bug the score cannot exceed BugTypeImportance.
  int result = trace->get_bug_type_importance();

  if (trace->get_length() == 0) {
    // Empty trace, we simply return the importance of the bug type
    // Empty trace may exist when the bug is reported by non-GVFG method and
    // none of the steps can be modelled into GVFG nodes
    return result;
  }

  if (is_dominated) {
    // Dominated trace are less important because we have already checked other
    // traces with same bug
    result = result * DOMINATION_FACTOR;
  }

  // Finally, we consider the confidence score of the trace
  result = score_with_confidence(result, confidence);

  return result;
}

// initialize the reports of results, report_insts are the reports stored in a
// kvec
GSAFTraceScorer::GSAFTraceScorer(std::vector<GuardedValueFlowTrace *> &reports,
                                 PackedTypeLayout *DL, DebugInfoAnalysis *DIA,
                                 GSAFChecker *CG, GSAFChecker *dom_tree_pass,
                                 GSAFModels *memory_spec, GSAFModels *IO_spec,
                                 GSAFModels *taint_spec, tactic_t tactic)
    : DL(DL), DIA(DIA), CG(CG), dom_pass(dom_tree_pass),
      memory_spec(memory_spec), IO_spec(IO_spec), taint_spec(taint_spec),
      tactic(tactic) {

  int report_size = reports.size();
  report_items.resize(report_size);

  //  auto *PassMgr = CBPassMgr::get_manager();
  //  DIA = PassMgr->get_analysis<DebugInfoAnalysis>();
  //  DL = PassMgr->get_analysis<PackedTypeLayout>();
  //  memory_spec = PassMgr->get_analysis<GSAFModels>();
  //  taint_spec = PassMgr->get_analysis<GSAFModels>();

  for (int i = 0; i < report_size; i++) {
    GuardedValueFlowTrace *trace = reports[i];

    report_items[i].trace = trace;
    report_items[i].is_dominated =
        false;                       // all results are initialized to be false;
    report_items[i].is_valid = true; // all results are not pruned by validation
                                     // checking before scoring;
    report_items[i].confidence =
        1.0; // all results are initialized as full confidence
    report_items[i].trace_confidence =
        1.0; // all results are initialized as full confidence
    report_items[i].path_confidence =
        1.0; // all results are initialized as full confidence
    report_items[i].context_confidence =
        1.0; // all results are initialized as full confidence
    report_index[trace] = i;
  }
}

GSAFTraceScorer::~GSAFTraceScorer() {}

void GSAFTraceScorer::dump_statistical_info(raw_ostream &O) {
  int n_dom = 0;
  int n_removed = 0;
  int report_size = report_items.size();

  for (int i = 0; i < report_size; i++) {
    if (report_items[i].is_dominated == true) {
      n_dom++;
    }

    if (!report_items[i].is_valid) {
      n_removed++;
    }
  }
  O << "Total reports: " << report_size << " and " << n_dom
    << " are removed by domination check\n";
  O << "Total reports clustered by value: " << report_size << " and "
    << n_removed << " are removed by domination check\n";
  O << "Total clusters by source: " << get_clustered_trace().get_num_clusters()
    << "\n";
}

void GSAFTraceScorer::dominate_checking() {
  // First, We collect the domination info
  do_dominate_checking_on_gvfg_node();
  int report_size = report_items.size();

  // Second, we collect the clustered trace
  for (int i = 0; i < report_size; i++) {
    if (true || is_valid(i)) {
      // TODO currently, we disabled validation check to test the effectiveness
      // of
      //  path sensitive checking
      GuardedValueFlowTrace *trace = report_items[i].trace;
      bool dom = is_dominated(i);
      if (trace) {
        clustered_trace.insert(*trace, dom);
      }
    }
  }

  if (scorer_dump_clustered_trace) {
    clustered_trace.dump(outs(), true, DIA, this);
  }
}

void GSAFTraceScorer::trace_confidence_checking() {
  int report_size = report_items.size();
  for (int i = 0; i < report_size; i++) {
    GuardedValueFlowTrace *trace = report_items[i].trace;
    if (trace) {
      float &target = report_items[i].trace_confidence;

      size_t start_index = 0;
      auto curr_obj_index_pair =
          trace->find(start_index, [](const GuardedValueFlowObject *obj) {
            return obj && isa<GuardedValueFlowNode>(obj);
          });

      while (curr_obj_index_pair.second < trace->get_length()) {
        auto next_obj_index_pair =
            trace->find(curr_obj_index_pair.second + 1,
                        [](const GuardedValueFlowObject *obj) {
                          return obj && isa<GuardedValueFlowNode>(obj);
                        });

        auto *curr_obj = curr_obj_index_pair.first;
        auto *next_obj = next_obj_index_pair.first;

        if (next_obj) {
          if (((const GuardedValueFlowNode *)curr_obj)
                  ->containsParent((const GuardedValueFlowNode *)next_obj)) {
            target *=
                ((const GuardedValueFlowNode *)curr_obj)
                    ->getConfidence((const GuardedValueFlowNode *)next_obj);
          } else if (((const GuardedValueFlowNode *)next_obj)
                         ->containsParent(
                             (const GuardedValueFlowNode *)curr_obj)) {
            target *=
                ((const GuardedValueFlowNode *)next_obj)
                    ->getConfidence((const GuardedValueFlowNode *)curr_obj);
          }
        }

        curr_obj_index_pair = std::move(next_obj_index_pair);
      }
    }
  }
}

static void collect_enclosing_func_info(
    GuardedValueFlowTrace &trace, GSAFChecker *CG,
    unordered_set<Function *> &func_in_trace,
    unordered_set<Function *> &func_in_trace_without_caller) {
  func_in_trace.clear();
  func_in_trace_without_caller.clear();
  for (int i = trace.getValidStartIdx(); i < trace.get_length(); i++) {
    const GuardedValueFlowObject *cur_step = trace[i];

    if (!cur_step)
      continue;

    Function *func = cur_step->getGraph()->getBaseFunc();
    func_in_trace.insert(func);
    func_in_trace_without_caller.insert(func);
  }

  for (Function *func : func_in_trace) {
    auto *graph = CG->getGraph(func);
    if (!graph)
      continue;
    for (auto *site : callSites(graph))
      for (auto *callee : site->getCallees())
        func_in_trace_without_caller.erase(callee);
  }
}

static void collect_phi_gated_info(
    GuardedValueFlowTrace &trace,
    unordered_map<
        Function *,
        unordered_map<const GuardedValueFlowPhiNode *,
                      unordered_set<const GuardedValueFlowPhiNode::Incoming *>>>
        &result) {

  result.clear();

  const GuardedValueFlowObject *last_step = nullptr;
  for (int i = trace.getValidStartIdx(); i < trace.get_length(); i++) {
    const GuardedValueFlowObject *cur_step = trace[i];

    if (!cur_step)
      continue;

    BasicBlock *cur_bb = cur_step->getParentBasicBlock();
    Function *cur_func = cur_bb->getParent();

    unordered_map<const GuardedValueFlowPhiNode *,
                  unordered_set<const GuardedValueFlowPhiNode::Incoming *>>
        &cur_func_phi_gated = result[cur_func];

    if (last_step) {
      if (const GuardedValueFlowPhiNode *cur_phi_node =
              dyn_cast<GuardedValueFlowPhiNode>(cur_step)) {
        if (const GuardedValueFlowNode *last_gated =
                dyn_cast<GuardedValueFlowNode>(last_step)) {
          for (const GuardedValueFlowPhiNode::Incoming &n : *cur_phi_node) {
            if (n.value_node == last_gated) {
              cur_func_phi_gated[cur_phi_node].insert(&n);
            }
          }
        }
      }
    }

    last_step = cur_step;
  }
}

static bool has_path_correlate_issue(BasicBlock *bb1, BasicBlock *bb2,
                                     GSAFChecker *dom_pass) {
  if (bb1 == nullptr || bb2 == nullptr) {
    return false;
  }

  if (bb1->getParent() != bb2->getParent()) {
    return false;
  }

  if (bb2 == bb1) {
    return false;
  }

  if (!dom_pass) {
    return false;
  }

  Function *func = bb1->getParent();

  const auto &dom_tree = dom_pass->getDomTree(func);
  const auto &post_dom_tree = dom_pass->getPostDomTree(func);

  if (dom_tree.dominates(bb1, bb2) || dom_tree.dominates(bb2, bb1) ||
      post_dom_tree.dominates(bb1, bb2) || post_dom_tree.dominates(bb2, bb1)) {
    // Has domination relation => no correlation issue
    return false;
  }

  return true;
}

static float get_path_confidence(GuardedValueFlowTrace *trace, GSAFChecker *CG,
                                 GSAFChecker *dom_pass) {
  float ret = 1.0;

  if (CG == nullptr) {
    outs() << "No CG is provided to scorer, running partial path confidence "
              "checking!!!\n";
  }

  unordered_set<Function *> func_in_trace_without_caller;
  unordered_set<Function *> func_in_trace;

  unordered_map<Function *, unordered_set<BasicBlock *>> bb_cache;
  // Cache the phi choices in trace
  // GuardedValueFlowPhiNode : the PHI node, IncomeNode* : the choice
  unordered_map<
      Function *,
      unordered_map<const GuardedValueFlowPhiNode *,
                    unordered_set<const GuardedValueFlowPhiNode::Incoming *>>>
      phi_gated_in_trace;
  unordered_map<Function *, const GuardedValueFlowGraph *> seg_cache;

  unordered_set<BasicBlock *> key_bbs;

  // Cache temp sets of phi nodes whose path condition cannot be
  //       directly modelled using the basic-blocks in the root
  // For example :
  //       L1 int *a = null;
  //       L2 if(cond) {
  //       L3     a = new int;
  //       L4 *a
  //
  // L1->L4 depend on that L3 is not executed
  //
  std::vector<const GuardedValueFlowPhiNode *> gated_phi_cache;

  float correlation_confidence = 0.0f;
  float path_feasibility_confidence = 1.0f;

  if (trace && trace->get_length() > 0) {
    // init caches
    func_in_trace_without_caller.clear();
    func_in_trace.clear();

    bb_cache.clear();

    phi_gated_in_trace.clear();

    if (CG) {
      collect_enclosing_func_info(*trace, CG, func_in_trace,
                                  func_in_trace_without_caller);
    }

    collect_phi_gated_info(*trace, phi_gated_in_trace);

    for (int j = trace->getValidStartIdx(); j < trace->get_length(); j++) {
      const GuardedValueFlowObject *cur_step = (*trace)[j];

      if (!cur_step)
        continue;

      BasicBlock *cur_bb = cur_step->getParentBasicBlock();
      Function *cur_func = cur_bb->getParent();
      const GuardedValueFlowGraph *parent_seg = cur_step->getGraph();

      bb_cache[cur_func].insert(cur_bb);
      seg_cache[cur_func] = parent_seg;
    }

    bool is_contradict = false;

    for (auto &func_info : bb_cache) {
      Function *func = func_info.first;
      auto &visited_bbs = func_info.second;

      const auto &dom_tree = dom_pass->getDomTree(func);
      const auto &post_dom_tree = dom_pass->getPostDomTree(func);

      GuardedValueFlowGraph *parent_seg = nullptr;

      unordered_map<const GuardedValueFlowPhiNode *,
                    unordered_set<const GuardedValueFlowPhiNode::Incoming *>>
          &cur_func_phi_gated = phi_gated_in_trace[func];

      if (seg_cache.count(func)) {
        parent_seg = const_cast<GuardedValueFlowGraph *>(seg_cache[func]);
      }

      if (parent_seg) {
        // Light weight finding of path feasibility using mini-solver
        std::vector<GuardedValueFlowRegionNode *> bb_regions;
        for (auto iter = visited_bbs.begin(); iter != visited_bbs.end();
             iter++) {
          BasicBlock *bb = *iter;
          GuardedValueFlowRegionNode *bb_region = parent_seg->findRegion(bb);
          if (bb_region) {
            bb_regions.push_back(bb_region);
          }
        }

        for (auto &iter : cur_func_phi_gated) {
          unordered_set<const GuardedValueFlowPhiNode::Incoming *> &choices =
              iter.second;
          if (choices.size() == 1) {
            // simple mini-solver does not consider "or" relations
            // Therefore only single-choice phi-gated function is considered
            // here
            const GuardedValueFlowPhiNode::Incoming *choice =
                *(choices.begin());
            assert(choice && "invalid phi gated function found");
            if (choice->condition_node) {
              GuardedValueFlowRegionNode *cond = parent_seg->findUnitRegion(
                  choice->condition_node, choice->condition_sense);
              if (cond) {
                bb_regions.push_back(cond);
              }
            }
          }
        }

        is_contradict = hasContradictoryRegions(bb_regions);

        if (is_contradict) {
          correlation_confidence = 1;
          path_feasibility_confidence = 0;
          break;
        }
      }

      // Correlation checking
      gated_phi_cache.clear();
      for (auto &phi_iter : cur_func_phi_gated) {
        const GuardedValueFlowPhiNode *phi_node = phi_iter.first;
        unordered_set<const GuardedValueFlowPhiNode::Incoming *> &choices =
            phi_iter.second;
        for (const GuardedValueFlowPhiNode::Incoming *choice : choices) {
          BasicBlock *phi_BB = phi_node->getParentBasicBlock();
          BasicBlock *choice_BB = choice->incoming_block;
          if (dom_tree.dominates(choice_BB, phi_BB)) {
            gated_phi_cache.push_back(phi_node);
            break;
          }
        }
      }

      // We suppose different phi-gated function has correlation issue
      int gated_phi_count = gated_phi_cache.size();
      int phi_correlation_count = gated_phi_count * (gated_phi_count - 1) / 2;
      for (int j = 0; j < phi_correlation_count; j++) {
        correlation_confidence = 1 - (1 - correlation_confidence) *
                                         (1 - PATH_CORRELATION_CONFIDENCE);
      }

      // Correlation amount BBs, between BB and PHI gated functions
      for (auto iter1 = visited_bbs.begin(); iter1 != visited_bbs.end();
           iter1++) {
        BasicBlock *bb1 = *iter1;
        for (auto iter2 = iter1; iter2 != visited_bbs.end(); iter2++) {
          if (iter2 == iter1) {
            // Same BB, we ignore
            continue;
          }

          BasicBlock *bb2 = *iter2;

          if (has_path_correlate_issue(bb1, bb2, dom_pass)) {
            correlation_confidence = 1 - (1 - correlation_confidence) *
                                             (1 - PATH_CORRELATION_CONFIDENCE);
          }
        }

        for (const GuardedValueFlowPhiNode *phi_node : gated_phi_cache) {
          BasicBlock *bb2 = phi_node->getParentBasicBlock();

          if (has_path_correlate_issue(bb1, bb2, dom_pass)) {
            correlation_confidence = 1 - (1 - correlation_confidence) *
                                             (1 - PATH_CORRELATION_CONFIDENCE);
          }
        }
      }

      if (CG && (!func_in_trace_without_caller.count(func)) &&
          func->getName().compare("main") != 0) {
        // Path feasibility checking only made in callee functions
        key_bbs.clear();

        for (auto iter = visited_bbs.begin(); iter != visited_bbs.end();
             iter++) {
          BasicBlock *bb = *iter;
          while (bb) {
            BasicBlock *dom_bb = immediateDominator(dom_tree, bb);
            BasicBlock *key_bb = bb;

            while (dom_bb && post_dom_tree.dominates(bb, dom_bb)) {
              key_bb = dom_bb;
              dom_bb = immediateDominator(dom_tree, dom_bb);
            }
            key_bbs.insert(key_bb);
            bb = dom_bb;
          }
        }

        int num_path_condition = key_bbs.size() - 1;
        for (int i = 0; i < num_path_condition + trace->getNumAdditionalConds();
             i++) {
          path_feasibility_confidence =
              path_feasibility_confidence * PATH_FEASIBLE_CONFIDENCE;
        }

        for (auto &iter : cur_func_phi_gated) {
          float cur_phi_feasible_confidence = 1.0f;
          bool phi_choice_concerned = false;

          const GuardedValueFlowPhiNode *phi_node = iter.first;
          unordered_set<const GuardedValueFlowPhiNode::Incoming *> &choices =
              iter.second;

          for (const GuardedValueFlowPhiNode::Incoming *choice : choices) {
            BasicBlock *phi_BB = phi_node->getParentBasicBlock();
            BasicBlock *choice_BB = choice->incoming_block;
            if (dom_tree.dominates(choice_BB, phi_BB)) {
              // A gated function not concerned detected
              if (phi_choice_concerned) {
                cur_phi_feasible_confidence =
                    1 - (1 - cur_phi_feasible_confidence) *
                            (1 - PATH_FEASIBLE_CONFIDENCE);
              } else {
                cur_phi_feasible_confidence = PATH_FEASIBLE_CONFIDENCE;
              }
            }
          }

          path_feasibility_confidence =
              path_feasibility_confidence * cur_phi_feasible_confidence;
        }
      }
    }

    ret = (1 - correlation_confidence) * path_feasibility_confidence;
  }

  return ret;
}

void GSAFTraceScorer::path_confidence_checking() {
  int report_size = report_items.size();
  for (int i = 0; i < report_size; i++) {
    GuardedValueFlowTrace *trace = report_items[i].trace;
    report_items[i].path_confidence = get_path_confidence(trace, CG, dom_pass);
  }
}

// Return true if the function func is a library function that
//        may have effect on bug report trace by making it infeasible
static bool is_effective_library_function(Function *func,
                                          GSAFModels *memory_spec,
                                          GSAFModels *taint_spec) {
  if (func->isDeclaration() && (!func->isIntrinsic()) &&
      (!memory_spec->isAllocFunc(func)) &&
      taint_spec->isFunctionArgAsSource(func)) {
    return true;
  }
  return false;
}

// Collect the path conditions of GVFG trace t as a vector of region nodes and
// store the result to res.
static void get_path_conds(GuardedValueFlowTrace &t,
                           std::vector<GuardedValueFlowRegionNode *> &res) {
  unordered_map<
      Function *,
      unordered_map<const GuardedValueFlowPhiNode *,
                    unordered_set<const GuardedValueFlowPhiNode::Incoming *>>>
      phi_gated;

  collect_phi_gated_info(t, phi_gated);
  for (auto &func_phigated_pair : phi_gated) {
    unordered_map<const GuardedValueFlowPhiNode *,
                  unordered_set<const GuardedValueFlowPhiNode::Incoming *>>
        &func_phi_gated = func_phigated_pair.second;
    for (auto &phinode_incomenode_pair : func_phi_gated) {
      unordered_set<const GuardedValueFlowPhiNode::Incoming *> &incoming_nodes =
          phinode_incomenode_pair.second;

      if (incoming_nodes.size() == 1) {
        // SMT mini-solver only works on "AND" operations and thus, we only
        // handle and conditions
        for (const GuardedValueFlowPhiNode::Incoming *incoming_node :
             incoming_nodes) {
          GuardedValueFlowNode *cond_node = incoming_node->condition_node;
          bool cond = incoming_node->condition_sense;
          if (cond_node) {
            GuardedValueFlowRegionNode *region =
                cond_node->getGraph()->findUnitRegion(cond_node, cond);
            if (region) {
              res.push_back(region);
            }
          }
        }
      }
    }
  }

  int num_steps = t.get_length();
  for (int i = 0; i < num_steps; i++) {
    if (t[i]) {
      GuardedValueFlowRegionNode *path_cond =
          t[i]->getGraph()->findRegion(t[i]->getParentBasicBlock());
      if (path_cond) {
        res.push_back(path_cond);
      }
    }
  }

  int num_additional_cond = t.getNumAdditionalConds();
  for (int i = 0; i < num_additional_cond; i++) {
    res.push_back(t.getAdditionalCond(i));
  }
}

void GSAFTraceScorer::process_step_for_context(
    std::vector<const GuardedValueFlowNode *> &worklist,
    unordered_set<Function *> &func_in_trace_without_caller,
    unordered_set<Function *> &func_in_trace,
    unordered_set<const GuardedValueFlowObject *> &processed_nodes,
    unordered_set<const GuardedValueFlowNode *> *callee_arg_dep,
    std::vector<GuardedValueFlowRegionNode *> path_choices,
    float &caller_confidence, float &callee_confidence,
    bool is_caller_confidence_enabled, bool is_callee_confidence_enabled,
    bool is_library_considered, float caller_unit_confidence,
    float callee_unit_confidence, bool is_ignore_phi) {
  // Each value from other function shall be considered effective to the trace
  // at most once Thus, we apply a cache processed_nodes to capture which value
  // has been processed Value_nodes is used to cache the temporally visited
  // nodes to avoid redundant processing Value flows has higher priority than
  // condition dependencies, i.e. we first consider value flows and then, we
  // consider condition dependencies
  unordered_set<const GuardedValueFlowObject *> value_nodes;

  // cache the arg_dep
  // callee_arg_dep is updated only when the step is not directly affected or
  // step granularity is not enabled Thus, we first cache the arg_dep and update
  // to the final arg_dep if required
  unordered_set<const GuardedValueFlowNode *> callee_arg_dep_tmp;

  // We assume each step shall only be affected by caller/callee once
  bool is_affected_by_caller = false;
  bool is_affected_by_callee = false;

  std::vector<const GuardedValueFlowNode *> children_cache;

  while (!worklist.empty()) {
    const GuardedValueFlowNode *processing_node = worklist.back();
    worklist.pop_back();

    const GuardedValueFlowGraph *parent_seg = processing_node->getGraph();
    Function *func = parent_seg->getBaseFunc();

    bool is_callee_processed = false;
    Function *processed_callee = nullptr;

    if (!processed_nodes.count(processing_node)) {
      if (isa<GuardedValueFlowArgumentNode>(processing_node) &&
          is_caller_confidence_enabled) {
        if (func_in_trace_without_caller.count(func) &&
            func->getName().compare("main") != 0) {
          // We only inline callers with caller analyzed in the trace
          // The confidence computation completely applies probability theory
          bool is_consider = false;
          if (nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                         GuardedValueFlowArgumentNode>(processing_node) ||
              nodeOfKind<GuardedValueFlowNode::Kind::VariableArgument>(
                  processing_node)) {
            is_consider = true;
          } else if (const GuardedValueFlowArgumentNode *processing_arg =
                         nodeOfKind<GuardedValueFlowNode::Kind::PseudoArgument,
                                    GuardedValueFlowArgumentNode>(
                             processing_node)) {
            // Here, we regard the effect of arg->f->g as the effect of arg
            const gvfg::AccessPath &ap = processing_arg->getAccessPath();
            Value *val = ap.get_base_ptr();

            GuardedValueFlowNode *base_node = parent_seg->findNode(val);
            if (base_node) {
              if (val && isa<GlobalValue>(val)) {
                // Affected by global, we consider only once
                if (!processed_nodes.count(nullptr)) {
                  is_consider = true;
                  processed_nodes.insert(nullptr);
                }
              } else if (!processed_nodes.count(base_node)) {
                is_consider = true;
              }

              processed_nodes.insert(base_node);
            } else {
              is_consider = true;
            }
          }

          if (is_consider) {
            if ((!IS_STEP_GRANULARITY) || (!is_affected_by_caller)) {
              caller_confidence =
                  1 - (1 - caller_confidence) * (1 - caller_unit_confidence);

              is_affected_by_caller = true;
            }
          } else {
            if (!IS_STEP_GRANULARITY) {
              caller_confidence = 1 - (1 - caller_confidence) *
                                          (1 - caller_unit_confidence *
                                                   (1 - GROUP_CORRELATION));
            }
          }

          processed_nodes.insert(processing_node);

          if (IS_STEP_GRANULARITY && is_affected_by_caller &&
              is_affected_by_callee) {
            return;
          }
        }
      }

      if (isa<GuardedValueFlowCallOutputNode>(processing_node) ||
          nodeOfKind<GuardedValueFlowNode::Kind::CallSiteReturnSummary,
                     GuardedValueFlowCallSummaryNode>(processing_node)) {
        const GuardedValueFlowCallSite *graph_call_site = nullptr;
        GuardedValueFlowRegionNode *site_cond = nullptr;

        if (const GuardedValueFlowCallOutputNode *output_node =
                dyn_cast<GuardedValueFlowCallOutputNode>(processing_node)) {
          site_cond = output_node->getGraph()->findRegion(
              output_node->getParentBasicBlock());
          graph_call_site = callSite(output_node);
        } else if (const GuardedValueFlowCallSummaryNode *output_node =
                       nodeOfKind<
                           GuardedValueFlowNode::Kind::CallSiteReturnSummary,
                           GuardedValueFlowCallSummaryNode>(processing_node)) {
          site_cond = output_node->getGraph()->findRegion(
              output_node->getParentBasicBlock());
          graph_call_site = callSite(output_node);
        } else {
          // Dead code
        }

        if (graph_call_site && site_cond) {
          bool is_library_call = true;

          for (const auto *iter = graph_call_site->getCallees().begin();
               iter != graph_call_site->getCallees().end(); iter++) {
            Function *callee = *iter;

            if (callee != nullptr) {
              if (func_in_trace.count(callee)) {
                is_callee_processed = true;
                processed_callee = callee;
                is_library_call = false;
                break;
              } else if (!is_effective_library_function(callee, memory_spec,
                                                        taint_spec)) {
                is_library_call = false;
              }
            }
          }

          if (!is_callee_processed) {
            // Callee is not in the trace
            if (is_callee_confidence_enabled ||
                (is_library_call && is_library_considered)) {
              // We consider callee effects or callee is library whose effect
              // should be considered

              bool is_consider = false;
              if (!processed_nodes.count(graph_call_site)) {
                // each callsite is considered only once
                is_consider = true;
                processed_nodes.insert(graph_call_site);
              }

              if (is_consider) {
                if ((!IS_STEP_GRANULARITY) || (!is_affected_by_callee)) {
                  callee_confidence = 1 - (1 - callee_confidence) *
                                              (1 - callee_unit_confidence);

                  is_affected_by_callee = true;
                }
              } else {
                if (!IS_STEP_GRANULARITY) {
                  callee_confidence = 1 - (1 - callee_confidence) *
                                              (1 - callee_unit_confidence *
                                                       (1 - GROUP_CORRELATION));
                }
              }

              processed_nodes.insert(processing_node);

              if (IS_STEP_GRANULARITY && is_affected_by_caller &&
                  is_affected_by_callee) {
                return;
              }
            }
          }
        }
      }
    }

    if (isa<GuardedValueFlowCallOutputNode>(processing_node) ||
        nodeOfKind<GuardedValueFlowNode::Kind::CallSiteReturnSummary,
                   GuardedValueFlowCallSummaryNode>(processing_node)) {
      const GuardedValueFlowCallSite *graph_call_site = nullptr;
      GuardedValueFlowRegionNode *site_cond = nullptr;

      if (const GuardedValueFlowCallOutputNode *output_node =
              dyn_cast<GuardedValueFlowCallOutputNode>(processing_node)) {
        site_cond = output_node->getGraph()->findRegion(
            output_node->getParentBasicBlock());
        graph_call_site = callSite(output_node);
      } else if (const GuardedValueFlowCallSummaryNode *output_node =
                     nodeOfKind<
                         GuardedValueFlowNode::Kind::CallSiteReturnSummary,
                         GuardedValueFlowCallSummaryNode>(processing_node)) {
        site_cond = output_node->getGraph()->findRegion(
            output_node->getParentBasicBlock());
        graph_call_site = callSite(output_node);
      } else {
        // Dead code
      }

      if (graph_call_site && site_cond) {
        if (!hasContradictoryRegions(path_choices, site_cond)) {
          std::vector<Function *> callees;

          if (is_callee_processed) {
            assert(processed_callee &&
                   "Invalid callee found in context confidence checking");
            callees.push_back(processed_callee);
          } else {
            for (const auto *callee_iter =
                     graph_call_site->getCallees().begin();
                 callee_iter != graph_call_site->getCallees().end();
                 callee_iter++) {
              Function *callee_func = *callee_iter;
              callees.push_back(callee_func);
            }
          }

          for (Function *callee_func : callees) {
            for (auto input_iter = graph_call_site->input_begin(callee_func);
                 input_iter != graph_call_site->input_end(callee_func);
                 input_iter++) {
              const GuardedValueFlowCallSiteInput &input_struct = *input_iter;
              GuardedValueFlowNode *arg_node = input_struct.InputNode;
              if (arg_node) {
                if (callee_arg_dep) {
                  // we update callee_arg_dep_tmp only when required, say
                  // callee_arg_dep is not null
                  callee_arg_dep_tmp.insert(arg_node);
                } else {
                  if (!value_nodes.count(arg_node)) {
                    value_nodes.insert(arg_node);
                    worklist.push_back(arg_node);
                  }
                }
              }
            }
          }
        }
      }
    }

    if (isa<GuardedValueFlowPhiNode>(processing_node) && is_ignore_phi) {
      // We do not consider phi-node,
      // whose path selection choice shall be represented by other steps
      continue;
    }

    children_cache.clear();

    unsigned n_children = processing_node->getNumChildren();
    for (unsigned i = 0; i < n_children; i++) {
      const GuardedValueFlowNode *child = processing_node->getChild(i);

      GuardedValueFlowRegionNode *node_cond = child->getRegion();
      if (hasContradictoryRegions(path_choices, node_cond)) {
        continue;
      }

      if (const GuardedValueFlowNode *parent_as_load_mem =
              nodeOfKind<GuardedValueFlowNode::Kind::LoadMemory,
                         GuardedValueFlowNode>(processing_node)) {
        GuardedValueFlowRegionNode *val_flow_cond =
            parent_as_load_mem->getMatchingRegion(child);
        if (hasContradictoryRegions(path_choices, val_flow_cond)) {
          continue;
        }
      }

      children_cache.push_back(child);
    }

    if (const GuardedValueFlowRegionNode *region_node =
            dyn_cast<GuardedValueFlowRegionNode>(processing_node)) {
      const GuardedValueFlowNode *cond_node = region_node->getConditionNode();
      if (cond_node) {
        children_cache.push_back(cond_node);
      }
    }

    for (const GuardedValueFlowNode *child : children_cache) {
      if (!value_nodes.count(child)) {
        value_nodes.insert(child);
        worklist.push_back(child);
      }
    }
  }

  // Finally, we update the final callee arg dep set
  // If it goes here, we assure that either the step is not directly affected or
  // step granularity is not enabled
  if (callee_arg_dep) {
    for (const GuardedValueFlowNode *arg_dep : callee_arg_dep_tmp) {
      callee_arg_dep->insert(arg_dep);
    }
  }
}

static void compute_value_sink_context_confidence(
    GuardedValueFlowTrace &trace,
    unordered_set<Function *> &func_in_trace_without_caller,
    unordered_set<Function *> &func_in_trace, GSAFChecker *CG,
    bool consider_direct, bool summary_only, float &callee_sink_confidence,
    float &caller_sink_confidence) {
  std::vector<GuardedValueFlowRegionNode *> path_conds;
  get_path_conds(trace, path_conds);

  std::vector<const GuardedValueFlowNode *> worklist;
  unordered_map<const GuardedValueFlowObject *, float> key_nodes_callee;
  unordered_map<const GuardedValueFlowObject *, float> key_nodes_caller;
  unordered_map<const GuardedValueFlowObject *, float> processed_nodes;

  for (int i = trace.getValidStartIdx(); i <= trace.getLEVFSrcIdx(); i++) {
    const GuardedValueFlowObject *cur_step = trace[i];

    if (!cur_step)
      continue;

    if (!(trace.getStepType(i) & GuardedValueFlowTrace::STY_VALUE)) {
      // For steps whose value is not cared, we do not care about whether the
      // value is passed to caller/callee
      continue;
    }

    if (const GuardedValueFlowNode *cur_node =
            dyn_cast<GuardedValueFlowNode>(cur_step)) {
      worklist.clear();
      worklist.push_back(cur_node);
      processed_nodes[cur_node] = 1.0f;

      while (!worklist.empty()) {
        const GuardedValueFlowNode *processing_node = worklist.back();
        worklist.pop_back();
        float cur_node_confidence = processed_nodes[processing_node];

        const GuardedValueFlowGraph *parent_seg = processing_node->getGraph();
        Function *func = parent_seg->getBaseFunc();

        // First, test if processing node is passed to callee
        Instruction *callee_site = nullptr;

        float path_confidence_factor =
            isa<GuardedValueFlowPhiNode>(processing_node)
                ? PATH_FEASIBLE_CONFIDENCE
                : 1.0f;
        float pass_to_callee_confidence_factor = 1.0f;

        if (const GuardedValueFlowCallSummaryNode *arg_summary =
                nodeOfKind<GuardedValueFlowNode::Kind::CallSiteArgumentSummary,
                           GuardedValueFlowCallSummaryNode>(processing_node)) {
          callee_site = arg_summary->getCallSite();
          pass_to_callee_confidence_factor = SUMMARY_CONFIDENCE;
        } else if (!summary_only) {
          for (const auto *iter = processing_node->useSites().begin();
               iter != processing_node->useSites().end(); iter++) {
            const GuardedValueFlowSite *site = *iter;

            if (site) {
              GuardedValueFlowRegionNode *site_cond =
                  site->getGraph()->findRegion(site->getParentBasicBlock());
              if (hasContradictoryRegions(path_conds, site_cond)) {
                continue;
              }
            }

            if (const GuardedValueFlowCallSite *seg_call_site =
                    dyn_cast<GuardedValueFlowCallSite>(site)) {
              if (!consider_direct) {
                bool is_directly_passed = false;
                for (auto *operand_node : seg_call_site->getCommonInputs())
                  if (operand_node == processing_node)
                    is_directly_passed = true;

                if (is_directly_passed) {
                  continue;
                }
              }

              callee_site = seg_call_site->getInstruction();
              break;
            }
          }
          pass_to_callee_confidence_factor = 1.0f;
        }

        if (callee_site) {
          // Passed to callee, we check if the callee is in the trace
          // we only consider values that is passed to a callee that is not
          // processed
          bool is_callee_processed_in_trace = false;

          auto *graph = CG->getGraph(func);
          if (auto *site = graph ? graph->findCallSite(callee_site) : nullptr)
            for (auto *callee : site->getCallees())
              if (callee && func_in_trace.count(callee)) {
                is_callee_processed_in_trace = true;
                break;
              }

          if (!is_callee_processed_in_trace) {
            float processing_node_confidence =
                processed_nodes[processing_node] *
                pass_to_callee_confidence_factor;
            if ((!key_nodes_callee.count(processing_node)) ||
                key_nodes_callee[processing_node] <
                    processing_node_confidence) {
              key_nodes_callee[processing_node] = processing_node_confidence;
            }
          }
        }

        // Second, test if processing node is passed to caller
        bool is_returned = false;

        if (parent_seg->isSummaryReturn(processing_node)) {
          is_returned = true;
        } else if (!summary_only) {
          for (const auto *iter = processing_node->useSites().begin();
               iter != processing_node->useSites().end(); iter++) {
            auto *site = *iter;

            if (site) {
              GuardedValueFlowRegionNode *site_cond =
                  site->getGraph()->findRegion(site->getParentBasicBlock());
              if (hasContradictoryRegions(path_conds, site_cond)) {
                continue;
              }
            }

            if (isa<GuardedValueFlowReturnSite>(site)) {
              if (!consider_direct) {
                if (nodeOfKind<GuardedValueFlowNode::Kind::CommonReturn,
                               GuardedValueFlowReturnNode>(processing_node)) {
                  continue;
                }
              }
              is_returned = true;
              break;
            }
          }
        }

        if (is_returned) {
          // Passed to caller, we check if the caller is in the trace
          // we only consider values that is passed to a caller that is not
          // processed
          if (func_in_trace_without_caller.count(func) &&
              func->getName().compare("main") != 0) {
            float processing_node_confidence = processed_nodes[processing_node];
            if ((!key_nodes_caller.count(processing_node)) ||
                key_nodes_caller[processing_node] <
                    processing_node_confidence) {
              key_nodes_caller[processing_node] = processing_node_confidence;
            }
          }
        }

        // Third, update worklist
        for (const auto *iter = processing_node->parents().begin();
             iter != processing_node->parents().end(); iter++) {
          const GuardedValueFlowNode *parent = iter->target;

          if (parent) {
            GuardedValueFlowRegionNode *node_cond = parent->getRegion();
            if (hasContradictoryRegions(path_conds, node_cond)) {
              continue;
            }

            if (const GuardedValueFlowNode *parent_as_load_mem =
                    nodeOfKind<GuardedValueFlowNode::Kind::LoadMemory,
                               GuardedValueFlowNode>(parent)) {
              GuardedValueFlowRegionNode *val_flow_cond =
                  parent_as_load_mem->getMatchingRegion(processing_node);
              if (hasContradictoryRegions(path_conds, val_flow_cond)) {
                continue;
              }
            }
          }

          float parent_confidence = processing_node->getConfidence(parent) *
                                    path_confidence_factor *
                                    cur_node_confidence;
          if ((!processed_nodes.count(parent)) ||
              processed_nodes[parent] < parent_confidence) {
            processed_nodes[parent] = parent_confidence;
            worklist.push_back(parent);
          }
        }
      }
    }
  }

  // compute the confidence
  for (auto &iter : key_nodes_callee) {
    float item_confidence = iter.second;
    float item_final_confidence =
        item_confidence * CALLEE_SINK_EFFECT_CONFIDENCE;
    callee_sink_confidence =
        1 - (1 - callee_sink_confidence) * (1 - item_final_confidence);
  }

  for (auto &iter : key_nodes_caller) {
    float item_confidence = iter.second;
    float item_final_confidence =
        item_confidence * CALLER_SINK_EFFECT_CONFIDENCE;
    caller_sink_confidence =
        1 - (1 - caller_sink_confidence) * (1 - item_final_confidence);
  }
}

void GSAFTraceScorer::context_confidence_checking() {

  if (CG == nullptr) {
    outs() << "No CG is provided to scorer, skipping context checking!!!\n";
    return;
  }

  int report_size = report_items.size();

  // Each value from other function shall be considered effective to the trace
  // at most once Thus, we apply a cache processed_nodes to capture which value
  // has been processed Value flows has higher priority than condition
  // dependencies, i.e. we first consider value flows and then, we consider
  // condition dependencies
  unordered_set<const GuardedValueFlowObject *> processed_nodes;
  std::vector<const GuardedValueFlowNode *> worklist;

  // Cache the phi choices in trace
  // GuardedValueFlowPhiNode : the PHI node, IncomeNode* : the choice
  unordered_map<
      Function *,
      unordered_map<const GuardedValueFlowPhiNode *,
                    unordered_set<const GuardedValueFlowPhiNode::Incoming *>>>
      phi_gated_in_trace;

  // Cache the functions in trace
  unordered_set<Function *> func_in_trace;
  unordered_set<Function *> func_in_trace_without_caller;

  // Cache the callee args as the data/control dependence of the trace
  unordered_set<const GuardedValueFlowNode *> callee_arg_val_dep;
  unordered_set<const GuardedValueFlowNode *> callee_arg_cond_dep;

  // Cache the dependence that callee arg depends on with a long dependence
  // chain, say such dependence may affect the feasibility of the trace, yet
  // with a very low probability
  unordered_set<const GuardedValueFlowNode *> callee_arg_val_deep_dep;
  unordered_set<const GuardedValueFlowNode *> callee_arg_cond_deep_dep;

  bool enable_caller_val = !(
      tactic & (TACTIC_FLAGS::NO_CALLER_CONTEXT | TACTIC_FLAGS::NO_CALLER_VAL));
  bool enable_callee_val = !(
      tactic & (TACTIC_FLAGS::NO_CALLEE_CONTEXT | TACTIC_FLAGS::NO_CALLEE_VAL));
  bool enable_caller_cond = !(tactic & (TACTIC_FLAGS::NO_CALLER_CONTEXT |
                                        TACTIC_FLAGS::NO_CALLER_COND));
  bool enable_callee_cond = !(tactic & (TACTIC_FLAGS::NO_CALLEE_CONTEXT |
                                        TACTIC_FLAGS::NO_CALLEE_COND));
  bool is_library_considered = !(tactic & TACTIC_FLAGS::CONTEXT_IGNORE_LIBRARY);

  for (int i = 0; i < report_size; i++) {
    GuardedValueFlowTrace *trace = report_items[i].trace;
    float &target = report_items[i].context_confidence;

    // path confidence is used to collect the importance of cond variables from
    // caller/callees
    float path_feasible_confidence = get_path_confidence(trace, CG, dom_pass) *
                                     PATH_PATH_CORRELATION_PERCENTAGE;

    if (trace && trace->get_length() != 0) {
      // Init info
      float cur_cond_from_callee_confidence = 0.0f;
      float cur_cond_from_caller_confidence = 0.0f;
      float cur_val_from_callee_confidence = 0.0f;
      float cur_val_from_caller_confidence = 0.0f;
      float cur_val_to_callee_confidence = 0.0f;
      float cur_val_to_caller_confidence = 0.0f;
      float cur_function_pointer_effect_confidence = 0.0f;

      processed_nodes.clear();

      func_in_trace.clear();
      func_in_trace_without_caller.clear();

      phi_gated_in_trace.clear();

      callee_arg_val_dep.clear();
      callee_arg_cond_dep.clear();

      callee_arg_val_deep_dep.clear();
      callee_arg_cond_deep_dep.clear();

      collect_enclosing_func_info(*trace, CG, func_in_trace,
                                  func_in_trace_without_caller);

      collect_phi_gated_info(*trace, phi_gated_in_trace);

      std::vector<GuardedValueFlowRegionNode *> path_conds;
      get_path_conds(*trace, path_conds);

      // Compute context confidence to values
      for (int j = trace->getValidStartIdx(); j <= trace->getLEVFSrcIdx();
           j++) {
        // Computing the context confidence for each trace step,
        //           and aggregate to a total result for each function
        const GuardedValueFlowObject *cur_step = (*trace)[j];

        if (!cur_step)
          continue;

        if (const GuardedValueFlowNode *cur_node =
                dyn_cast<GuardedValueFlowNode>(cur_step)) {
          worklist.clear();

          if (trace->getStepType(j) & GuardedValueFlowTrace::STY_VALUE) {
            worklist.push_back(cur_node);

            process_step_for_context(
                worklist, func_in_trace_without_caller, func_in_trace,
                processed_nodes, &callee_arg_val_dep, path_conds,
                cur_val_from_caller_confidence, cur_val_from_callee_confidence,
                enable_caller_val, enable_callee_val,
                false, // We never consider values directly generated from
                       // libraries, for value check
                CALLER_VAL_EFFECT_CONFIDENCE, CALLEE_VAL_EFFECT_CONFIDENCE,
                true);
          }
        } else if (const GuardedValueFlowCallSite *cur_node =
                       dyn_cast<GuardedValueFlowCallSite>(cur_step)) {
          // We first check if the callsite is an inter-face call site
          bool is_interface_call_site = false;

          const GuardedValueFlowObject *next_step = nullptr;
          const GuardedValueFlowObject *prev_step = nullptr;

          int next_idx = j + 1;
          int prev_idx = j - 1;
          while (next_idx < trace->get_length()) {
            next_step = (*trace)[next_idx];
            if (next_step)
              break;

            next_idx++;
          }

          if (next_step && next_step->getGraph() != cur_step->getGraph()) {
            is_interface_call_site = true;
          } else {
            while (prev_idx >= 0) {
              prev_step = (*trace)[prev_idx];
              if (prev_step)
                break;

              prev_idx--;
            }

            if (prev_step && prev_step->getGraph() != cur_step->getGraph()) {
              is_interface_call_site = true;
            }
          }

          if (is_interface_call_site) {
            auto *cs = cast<CallBase>(cur_node->getInstruction());
            for (auto *arg_iter = cs->arg_begin(); arg_iter != cs->arg_end();
                 arg_iter++) {
              Value *arg = *arg_iter;
              const GuardedValueFlowNode *arg_node =
                  cur_step->getGraph()->findNode(arg);
              if (arg_node) {
                callee_arg_val_dep.insert(arg_node);
              }
            }

            if (!(tactic & TACTIC_FLAGS::CONTEXT_IGNORE_FUNCTION_POINTER)) {
              bool is_single_callee = false;
              auto *graph =
                  CG->getGraph(cur_node->getParentBasicBlock()->getParent());
              if (auto *site = graph ? graph->findCallSite(cs) : nullptr) {
                size_t count = 0;
                for (auto *callee : site->getCallees())
                  if (callee)
                    ++count;
                is_single_callee = count == 1;
              }

              if (cs->getCalledFunction() == nullptr && !is_single_callee) {
                // Function pointer that is possible to cause inaccuracy
                // If a function pointer points to only one callee, we suppose
                // the result is accurate
                cur_function_pointer_effect_confidence =
                    1 - (1 - cur_function_pointer_effect_confidence) *
                            FUNCTION_POINTER_CORRECT_CONFIDENCE;
              }
            }
          }
        }
      }

      // Compute confidence to condition
      for (int j = trace->getValidStartIdx(); j < trace->get_length(); j++) {
        // Computing the context confidence for each trace step,
        //           and aggregate to a total result for each function
        const GuardedValueFlowObject *cur_step = (*trace)[j];

        if (!cur_step)
          continue;

        worklist.clear();

        if ((trace->getStepType(j) & GuardedValueFlowTrace::STY_VALUE) ||
            (trace->getStepType(j) & GuardedValueFlowTrace::STY_USE_SITE)) {
          if (const GuardedValueFlowNode *cur_node =
                  dyn_cast<GuardedValueFlowNode>(cur_step)) {
            // cond for nodes
            worklist.push_back(cur_node->getRegion());

            if (const GuardedValueFlowPhiNode *phi_node =
                    dyn_cast<GuardedValueFlowPhiNode>(cur_node)) {
              Function *cur_func = phi_node->getParentBasicBlock()->getParent();

              unordered_map<
                  const GuardedValueFlowPhiNode *,
                  unordered_set<const GuardedValueFlowPhiNode::Incoming *>>
                  &cur_func_phi_gated = phi_gated_in_trace[cur_func];

              auto choices_iter = cur_func_phi_gated.find(phi_node);

              if (choices_iter != cur_func_phi_gated.end()) {
                unordered_set<const GuardedValueFlowPhiNode::Incoming *>
                    &choices = choices_iter->second;
                for (const GuardedValueFlowPhiNode::Incoming *n : choices) {
                  if (n->condition_node)
                    worklist.push_back(n->condition_node);
                }
              }
            }
          } else if (const GuardedValueFlowSite *cur_node =
                         dyn_cast<GuardedValueFlowCallSite>(cur_step)) {
            // Cond for sites
            Instruction *site_inst = cur_node->getInstruction();
            if (site_inst != nullptr) {
              BasicBlock *site_bb = site_inst->getParent();
              GuardedValueFlowRegionNode *site_bb_cond =
                  cur_node->getGraph()->findRegion(site_bb);
              if (site_bb_cond != nullptr) {
                worklist.push_back(site_bb_cond);
              }
            }
          }

          process_step_for_context(
              worklist, func_in_trace_without_caller, func_in_trace,
              processed_nodes, &callee_arg_cond_dep, path_conds,
              cur_cond_from_caller_confidence, cur_cond_from_callee_confidence,
              enable_caller_cond, enable_callee_cond, is_library_considered,
              CALLER_COND_EFFECT_CONFIDENCE * (1 - path_feasible_confidence),
              CALLEE_COND_EFFECT_CONFIDENCE * (1 - path_feasible_confidence),
              false);
        }
      }

      // additional path conditions
      int num_additional_conds = trace->getNumAdditionalConds();
      for (int j = 0; j < num_additional_conds; j++) {
        GuardedValueFlowRegionNode *region = trace->getAdditionalCond(j);
        worklist.push_back(region);

        process_step_for_context(
            worklist, func_in_trace_without_caller, func_in_trace,
            processed_nodes, &callee_arg_cond_dep, path_conds,
            cur_cond_from_caller_confidence, cur_cond_from_callee_confidence,
            enable_caller_cond, enable_callee_cond, is_library_considered,
            CALLER_COND_EFFECT_CONFIDENCE * (1 - path_feasible_confidence),
            CALLEE_COND_EFFECT_CONFIDENCE * (1 - path_feasible_confidence),
            false);
      }

      // Compute the confidence on callee args that may affect the value in the
      // analyzed trace
      worklist.clear();
      for (const GuardedValueFlowNode *arg_node : callee_arg_val_dep) {
        worklist.push_back(arg_node);
      }

      process_step_for_context(
          worklist, func_in_trace_without_caller, func_in_trace,
          processed_nodes, &callee_arg_val_deep_dep, path_conds,
          cur_val_from_caller_confidence, cur_val_from_callee_confidence,
          enable_caller_val, enable_callee_val, is_library_considered,
          CALLER_VAL_EFFECT_CONFIDENCE * CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR,
          CALLEE_VAL_EFFECT_CONFIDENCE * CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR,
          false);

      // Compute the confidence on callee args that may affect the condition in
      // the analyzed trace
      worklist.clear();
      for (const GuardedValueFlowNode *arg_node : callee_arg_cond_dep) {
        worklist.push_back(arg_node);
      }

      process_step_for_context(
          worklist, func_in_trace_without_caller, func_in_trace,
          processed_nodes, &callee_arg_cond_deep_dep, path_conds,
          cur_cond_from_caller_confidence, cur_cond_from_callee_confidence,
          enable_caller_cond, enable_callee_cond, is_library_considered,
          CALLER_COND_EFFECT_CONFIDENCE * CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR *
              (1 - path_feasible_confidence),
          CALLEE_COND_EFFECT_CONFIDENCE * CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR *
              (1 - path_feasible_confidence),
          false);

      // Compute the confidence on callee args with long dependence chain
      //         that may affect the feasibility of the analyzed trace
      worklist.clear();
      for (const GuardedValueFlowNode *arg_node : callee_arg_val_deep_dep) {
        worklist.push_back(arg_node);
      }

      process_step_for_context(
          worklist, func_in_trace_without_caller, func_in_trace,
          processed_nodes, nullptr, path_conds, cur_val_from_caller_confidence,
          cur_val_from_callee_confidence, enable_caller_val, enable_callee_val,
          is_library_considered,
          CALLER_VAL_EFFECT_CONFIDENCE * CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR *
              CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR,
          CALLEE_VAL_EFFECT_CONFIDENCE * CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR *
              CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR,
          false);

      worklist.clear();
      for (const GuardedValueFlowNode *arg_node : callee_arg_cond_deep_dep) {
        worklist.push_back(arg_node);
      }

      process_step_for_context(
          worklist, func_in_trace_without_caller, func_in_trace,
          processed_nodes, nullptr, path_conds, cur_cond_from_caller_confidence,
          cur_cond_from_callee_confidence, enable_caller_cond,
          enable_callee_cond, is_library_considered,
          CALLER_COND_EFFECT_CONFIDENCE * CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR *
              CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR *
              (1 - path_feasible_confidence),
          CALLEE_COND_EFFECT_CONFIDENCE * CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR *
              CALLEE_ARG_EFFECT_CONFIDENCE_FACTOR *
              (1 - path_feasible_confidence),
          false);

      // Compute the confidence that the values in the value flow of the trace
      // may be passed to a callee/caller
      //         that is not in the trace
      if (tactic & TACTIC_FLAGS::CONTEXT_WITH_VALUE_SINK) {
        compute_value_sink_context_confidence(
            *trace, func_in_trace_without_caller, func_in_trace, CG,
            tactic & TACTIC_FLAGS::CONTEXT_VALUE_SINK_CONSIDER_DIRECT,
            tactic & TACTIC_FLAGS::CONTEXT_VALUE_SINK_SUMMARY_ONLY,
            cur_val_to_callee_confidence, cur_val_to_caller_confidence);
      }

      // Computing the context confidence for the whole trace
      target = (1 - cur_cond_from_callee_confidence) *
               (1 - cur_cond_from_caller_confidence) *
               (1 - cur_val_from_callee_confidence) *
               (1 - cur_val_from_caller_confidence) *
               (1 - cur_val_to_callee_confidence) *
               (1 - cur_val_to_caller_confidence) *
               (1 - cur_function_pointer_effect_confidence);

      if (scorer_dump_context_confidence &&
          trace->getKeyIdx() != GuardedValueFlowTrace::IDX_INVALID) {
        outs() << "Context confidence : (idx = " << i
               << "\t, vul = " << (*trace)[trace->getKeyIdx()] << ") "
               << "|format = callee/caller"
               << "|cond = "
               << format_str(CONFIDENCE_OUTPUT_FORMAT,
                             cur_cond_from_callee_confidence)
               << "/"
               << format_str(CONFIDENCE_OUTPUT_FORMAT,
                             cur_cond_from_caller_confidence)
               << "|val = "
               << format_str(CONFIDENCE_OUTPUT_FORMAT,
                             cur_val_from_callee_confidence)
               << "/"
               << format_str(CONFIDENCE_OUTPUT_FORMAT,
                             cur_val_from_caller_confidence)
               << "|sink = "
               << format_str(CONFIDENCE_OUTPUT_FORMAT,
                             cur_val_to_callee_confidence)
               << "/"
               << format_str(CONFIDENCE_OUTPUT_FORMAT,
                             cur_val_to_caller_confidence)
               << "|FP = "
               << format_str(CONFIDENCE_OUTPUT_FORMAT,
                             cur_function_pointer_effect_confidence)
               << "|PATH = "
               << format_str(CONFIDENCE_OUTPUT_FORMAT, path_feasible_confidence)
               << "|Final Result = "
               << format_str(CONFIDENCE_OUTPUT_FORMAT, target) << "|"
               << processed_nodes.size() << "\n";

        DEBUG_WITH_TYPE(
            "test_context_confidence",
            for (const GuardedValueFlowObject *node : processed_nodes) {
              Instruction *inst = node->getDebugInstruction();
              Value *val = node->getDebugValue();
              if (inst) {
                if (val) {
                  dbgs() << "++" << *val << "**\n";
                }
                dbgs() << "**" << DIA->getSourceLocation(inst) << "**\n";
              } else if (val) {
                dbgs() << "**" << *val << "**\n";
              } else {
                dbgs() << "**"
                       << "null"
                       << "**\n";
              }
            });

        if (const GuardedValueFlowObject *key_step =
                (*trace)[trace->getKeyIdx()]) {
          if (key_step) {
            if (Instruction *inst = key_step->getDebugInstruction()) {
              outs() << "[" << DIA->getSourceLocation(inst) << "]";
            } else {
              if (const GuardedValueFlowNode *seg_node =
                      dyn_cast<GuardedValueFlowNode>(key_step)) {
                outs() << seg_node->getDescription();
              } else {
                outs() << "**" << *key_step << "**";
              }
            }
          } else {
            outs() << "<NA>";
          }
          outs() << "\n";
        }
      }
    }
  }
}

static SMTExpr get_expr_for_node(const GuardedValueFlowNode *n,
                                 GSAFSolver &seg_solver, string &name_suffix) {
  SMTExpr n_var = seg_solver.getOrInsertExpr(n);
  SMTExprVec n_var_vec = seg_solver.getSMTFactory().createEmptySMTExprVec();
  n_var_vec.push_back(n_var);
  unordered_map<std::string, SMTExpr> mapping;
  SMTExprVec result =
      seg_solver.getSMTFactory().rename(n_var_vec, name_suffix, mapping).first;
  assert(result.size() == 1 && "Incorrect SMT expr renaming detected");
  return result[0];
}

static void push_cond(const GuardedValueFlowNode *n, GSAFSolver &seg_solver,
                      string &name_suffix) {
  unordered_map<std::string, SMTExpr> mapping;

  seg_solver.addAll(seg_solver.getSMTFactory()
                        .rename(seg_solver.getCtrlDeps(n), name_suffix, mapping)
                        .first);

  seg_solver.addAll(seg_solver.getSMTFactory()
                        .rename(seg_solver.getDataDeps(n), name_suffix, mapping)
                        .first);
}

// Link the arguments and return values of a callsite
// (1) Link formal input/output to actual input/output
// ret_inst is required because it is possible that only one ReturnInst should
// be linked
static void process_callsite_for_validity_checking(
    GSAFSolver &seg_solver, const GuardedValueFlowCallSite *callsite,
    const GuardedValueFlowGraph *seg_caller,
    const GuardedValueFlowGraph *seg_callee, ReturnInst *ret_inst,
    unordered_map<Function *, int> &name_suffix_map) {

  assert(callsite && "Invalid callsite passed to path validity checking");
  assert(seg_callee && seg_caller &&
         "Invalid caller/callee passed to path validity checking");
  assert(
      llvm::is_contained(callsite->getCallees(), seg_callee->getBaseFunc()) &&
      "Invalid callsite-callee mapping for path validity checking");

  unordered_map<std::string, SMTExpr> mapping;

  const GuardedValueFlowNode *n = callsite->getCommonOutput();

  Function *callee = seg_callee->getBaseFunc();
  Function *caller = seg_caller->getBaseFunc();

  if (name_suffix_map.find(caller) == name_suffix_map.end()) {
    name_suffix_map[caller] = 0;
  }

  if (name_suffix_map.find(callee) == name_suffix_map.end()) {
    name_suffix_map[callee] = 0;
  }

  string caller_suffix = format_str("_%s_%d", caller->getName().str().c_str(),
                                    name_suffix_map[caller]);
  string callee_suffix = format_str("_%s_%d", callee->getName().str().c_str(),
                                    name_suffix_map[callee]);

  if (n != nullptr && ret_inst != nullptr) {
    assert(ret_inst->getParent()->getParent() == callee &&
           "Unmatched callee function and return value");

    Value *ret_val = ret_inst->getReturnValue();
    const GuardedValueFlowNode *ret_node = seg_callee->findNode(ret_val);
    if (ret_node != nullptr) {
      SMTExpr n_var = get_expr_for_node(n, seg_solver, caller_suffix);
      SMTExpr ret_var = get_expr_for_node(ret_node, seg_solver, callee_suffix);

      SMTExpr Eq = n_var == ret_var;

      seg_solver.add(Eq);
      push_cond(ret_node, seg_solver, callee_suffix);
    } else {
      assert(false && "Unhandled return instruction in GVFG");
    }
  }

  // Value passed into a function call, we map all the arguments
  if (caller && callee) {
    for (auto actual_arg_iter = callsite->input_begin(callee);
         actual_arg_iter != callsite->input_end(callee); actual_arg_iter++) {
      const GuardedValueFlowCallSiteInput &input_struct = *actual_arg_iter;
      size_t idx = input_struct.InputIndex;
      bool is_common = input_struct.IsCommonInput;
      const GuardedValueFlowNode *actual_arg_node = input_struct.InputNode;

      const GuardedValueFlowNode *formal_arg_node = nullptr;
      if (is_common) {
        if (idx < seg_callee->getNumCommonArgument()) {
          formal_arg_node = seg_callee->getCommonArgument(idx);
        }
      } else {
        if (idx < seg_callee->getNumPseudoArgument()) {
          formal_arg_node = seg_callee->getPseudoArgument(idx);
        }
      }

      if (formal_arg_node && actual_arg_node) {
        SMTExpr actual_arg_expr =
            get_expr_for_node(actual_arg_node, seg_solver, caller_suffix);
        SMTExpr formal_arg_expr =
            get_expr_for_node(formal_arg_node, seg_solver, callee_suffix);

        if (actual_arg_expr.isSameSort(formal_arg_expr)) {
          // Link caller callee's arguments.
          SMTExpr Eq = actual_arg_expr == formal_arg_expr;
          seg_solver.add(Eq);
        } else {
          errs() << "GSAF scoring: "
                 << "The z3 type for arguments and parameters do not match\n";
          errs() << "GSAF scoring: " << "Inconsistency detected, maybe "
                 << caller->getName() << " does not have callee "
                 << callee->getName() << "\n";
        }

        push_cond(actual_arg_node, seg_solver, caller_suffix);
      }
    }

    for (auto *actual_ret_node : callOutputs(callsite, callee)) {
      const GuardedValueFlowNode *formal_ret_node = nullptr;
      if (nodeOfKind<GuardedValueFlowNode::Kind::CallSiteCommonOutput>(
              actual_ret_node)) {
        formal_ret_node = seg_callee->getCommonReturn();
      } else {
        assert((nodeOfKind<GuardedValueFlowNode::Kind::CallSitePseudoOutput,
                           GuardedValueFlowCallOutputNode>(actual_ret_node) &&
                "Unknown output node type"));
        size_t idx = actual_ret_node->getIndex();
        if (idx < seg_callee->pseudoReturns().size()) {
          formal_ret_node = seg_callee->getPseudoReturn(idx);
        }
      }

      if (formal_ret_node && actual_ret_node) {
        SMTExpr arg_expr =
            get_expr_for_node(actual_ret_node, seg_solver, caller_suffix);
        SMTExpr call_arg_expr =
            get_expr_for_node(formal_ret_node, seg_solver, callee_suffix);

        if (arg_expr.isSameSort(call_arg_expr)) {
          // Link caller callee's arguments.
          SMTExpr Eq = arg_expr == call_arg_expr;
          seg_solver.add(Eq);
        } else {
          errs() << "GSAF scoring: "
                 << "The z3 type for arguments and parameters do not match\n";
          errs() << "GSAF scoring: " << "Inconsistency detected, maybe "
                 << caller->getName() << " does not have callee "
                 << callee->getName() << "\n";
        }

        push_cond(formal_ret_node, seg_solver, callee_suffix);
      }
    }
  }
}

// Traversing all the traces and calculate whether each trace it valid using SMT
// solver
void GSAFTraceScorer::path_validity_checking() {
  // GVFG==nullptr means that we do not use path-sensitive checking
  if (DL == nullptr)
    return;

  int report_size = report_items.size();

  LLVM_DEBUG(errs() << "\n");

  for (int idx = 0; idx < report_size; idx++) {
    LLVM_DEBUG(errs() << "Path validation checking " << idx + 1 << "/"
                      << report_size << "                   \r");

    // skip invalid trace.
    if (report_items[idx].is_valid == false)
      continue;

    //		//We do not check dominated traces
    //		if (report_items[i].is_dominated == true) {
    //			report_items[i].is_valid = false;
    //			continue;
    //		}

    SMTFactory Fctry;
    GSAFSolver seg_solver(Fctry, CG->getModule()->getDataLayout());
    seg_solver.setModels(memory_spec, GSAFOptions::EnableHeapAllocFailure,
                         GSAFOptions::EnableFileAllocFailure);

    unordered_map<std::string, SMTExpr> mapping;

    if (!report_items[idx].trace)
      continue;

    GuardedValueFlowTrace &t = *report_items[idx].trace;

    // A map indicating the name suffix
    // VarName_Function->getName()_int
    // int is initialized as 0
    // The int variable is increased when the trace moves out of the function
    // and the value flow is NOT caller->callee Say, if cur_step.func !=
    // next_step.func && not_caller_to_callee_value_flow(cur_step, next_step),
    //    we perform name_suffix_map[cur_step.func]++
    unordered_map<Function *, int> name_suffix_map;

    for (int j = 0; j < t.get_length(); j++) {
      const GuardedValueFlowObject *cur_step = t[j];
      if (!cur_step) {
        continue;
      }

      bool is_caller_to_callee_value_flow = false;
      bool is_callee_to_caller_value_flow = false;

      const GuardedValueFlowObject *next_step = nullptr;
      int k = j + 1;
      while (k < t.get_length()) {
        next_step = t[k];
        if (next_step)
          break;
        k++;
      }

      const GuardedValueFlowGraph *cur_seg = cur_step->getGraph();
      Function *cur_func = cur_seg->getBaseFunc();

      if (name_suffix_map.find(cur_func) == name_suffix_map.end()) {
        name_suffix_map[cur_func] = 0;
      }

      string cur_suffix =
          format_str("_%s_%d", cur_func->getName().str().c_str(),
                     name_suffix_map[cur_func]);

      if (next_step) {
        // Try inline if required
        const GuardedValueFlowGraph *next_seg = next_step->getGraph();
        Function *next_func = next_seg->getBaseFunc();
        if (const GuardedValueFlowCallSite *call_site_node =
                dyn_cast<GuardedValueFlowCallSite>(cur_step)) {
          // Input : Caller(cur_step) -> Callee(next_step)
          is_caller_to_callee_value_flow = true;

          Function *called_function = call_site_node->getCalledFunction();
          if (called_function == nullptr || called_function == next_func) {
            auto *cs = cast<CallBase>(call_site_node->getInstruction());
            if (llvm::isPointerAnalysisCallsiteCompatible(
                    cs, next_seg->getBaseFunc()))
              process_callsite_for_validity_checking(seg_solver, call_site_node,
                                                     cur_seg, next_seg, nullptr,
                                                     name_suffix_map);
          }
        } else if (const GuardedValueFlowReturnSite *return_site =
                       dyn_cast<GuardedValueFlowReturnSite>(cur_step)) {
          // Output : Callee(cur_step) -> Caller(next_step)
          is_callee_to_caller_value_flow = true;

          ReturnInst *ret = dyn_cast<ReturnInst>(return_site->getInstruction());

          if (const GuardedValueFlowCallOutputNode *out_node =
                  dyn_cast<GuardedValueFlowCallOutputNode>(next_step)) {
            // Pseudo output
            const GuardedValueFlowCallSite *callsite_node = callSite(out_node);
            if (callsite_node &&
                llvm::is_contained(callsite_node->getCallees(), cur_func)) {
              auto *cs = cast<CallBase>(callsite_node->getInstruction());
              if (llvm::isPointerAnalysisCallsiteCompatible(
                      cs, cur_seg->getBaseFunc()))
                process_callsite_for_validity_checking(
                    seg_solver, callsite_node, next_seg, cur_seg, ret,
                    name_suffix_map);
            }
          } else {
            // Check if next step is a common return, i.e. a call instruction
            Instruction *inst = next_step->getDebugInstruction();
            if (inst) {
              if (CallInst *ci = dyn_cast<CallInst>(inst)) {
                const GuardedValueFlowGraph *base_seg = next_step->getGraph();
                GuardedValueFlowCallSite *callsite_node =
                    base_seg->findSite<GuardedValueFlowCallSite>(ci);
                if (callsite_node &&
                    llvm::is_contained(callsite_node->getCallees(), cur_func)) {
                  auto *cs = cast<CallBase>(callsite_node->getInstruction());
                  if (llvm::isPointerAnalysisCallsiteCompatible(
                          cs, cur_seg->getBaseFunc()))
                    process_callsite_for_validity_checking(
                        seg_solver, callsite_node, next_seg, cur_seg, ret,
                        name_suffix_map);
                }
              }
            }
          }
        }
      }

      // Adding Data Dependencies
      if (t.getStepType(j) & GuardedValueFlowTrace::STY_VALUE) {
        // We first insert the PHI-gated functions
        if (next_step) {
          if (const GuardedValueFlowPhiNode *seg_phi_node =
                  dyn_cast<GuardedValueFlowPhiNode>(next_step)) {
            bool is_real_phi_gated = false;

            for (auto &in_val : *seg_phi_node) {
              if (in_val.value_node == cur_step) {
                is_real_phi_gated = true;
                break;
              }
            }

            if (is_real_phi_gated) {
              if (const GuardedValueFlowNode *cur_node =
                      dyn_cast<GuardedValueFlowNode>(cur_step)) {
                SMTExprVec gated_function =
                    seg_solver.getPhiGated(seg_phi_node, cur_node);
                SMTExprVec gated_function_final =
                    seg_solver.getSMTFactory()
                        .rename(gated_function, cur_suffix, mapping)
                        .first;

                seg_solver.addAll(gated_function_final);

                SMTExpr phi_node_expr =
                    get_expr_for_node(seg_phi_node, seg_solver, cur_suffix);
                SMTExpr phi_choice_expr =
                    get_expr_for_node(cur_node, seg_solver, cur_suffix);

                seg_solver.add(phi_node_expr == phi_choice_expr);
              }
            }
          }
        }

        if (const GuardedValueFlowNode *cur_node =
                dyn_cast<GuardedValueFlowNode>(cur_step)) {
          seg_solver.addAll(
              seg_solver.getSMTFactory()
                  .rename(seg_solver.getDataDeps(cur_node), cur_suffix, mapping)
                  .first);
        }
      }

      // Adding Control dependencies
      if (t.getStepType(j) & GuardedValueFlowTrace::STY_USE_SITE) {
        seg_solver.addAll(
            seg_solver.getSMTFactory()
                .rename(seg_solver.getCtrlDeps(cur_step->getParentBasicBlock(),
                                               cur_step->getGraph()),
                        cur_suffix, mapping)
                .first);
      }

      // Revise function versions
      if (next_step) {
        Function *next_func = next_step->getGraph()->getBaseFunc();
        if (next_func != cur_func) {
          if (!is_caller_to_callee_value_flow) {
            int &cur_sig = name_suffix_map[cur_func];
            cur_sig++;
          } else if (!is_callee_to_caller_value_flow) {
            if (name_suffix_map.find(next_func) == name_suffix_map.end()) {
              name_suffix_map[next_func] = 0;
            }

            int &next_sig = name_suffix_map[next_func];
            next_sig++;
          }
        }
      }
    }

    // Adding Additional Conds
    int num_additional_conds = t.getNumAdditionalConds();
    for (int i = 0; i < num_additional_conds; i++) {
      GuardedValueFlowRegionNode *cond = t.getAdditionalCond(i);

      if (!cond)
        continue;

      Function *cur_func = cond->getGraph()->getBaseFunc();

      if (name_suffix_map.find(cur_func) == name_suffix_map.end()) {
        name_suffix_map[cur_func] = 0;
      }

      int cur_sig = name_suffix_map[cur_func];

      // TODO: Currently, we did not distinguish the additional conds for
      // different function instances and thus, we create a cond for each
      // instance
      for (int j = 0; j <= cur_sig; j++) {
        string cur_suffix =
            format_str("_%s_%d", cur_func->getName().str().c_str(), j);

        seg_solver.addAll(
            seg_solver.getSMTFactory()
                .rename(seg_solver.getDataDeps(cond), cur_suffix, mapping)
                .first);

        SMTExpr cond_expr = get_expr_for_node(cond, seg_solver, cur_suffix);
        seg_solver.add(cond_expr == true);
      }
    }

    LLVM_DEBUG(cerr << seg_solver << "\n"; fflush(stdout););
    SMTSolver::SMTResultType res = seg_solver.check();

    if (res == SMTSolver::SMTResultType::SMTRT_Unsat) {
      report_items[idx].is_valid = false;
      LLVM_DEBUG(errs() << DEBUG_TYPE << idx << ":"
                        << "unsat"
                        << "\n";
                 errs().flush(););
    } else if (res == SMTSolver::SMTResultType::SMTRT_Sat) {
      report_items[idx].is_valid = true;
      LLVM_DEBUG(errs() << DEBUG_TYPE << idx << ":"
                        << "sat"
                        << "\n";
                 errs().flush(););
    } else {
      // we do not prune traces that cannot be solved
      report_items[idx].is_valid = true;
      LLVM_DEBUG(errs() << DEBUG_TYPE << idx << ":"
                        << "sat"
                        << "\n";
                 errs().flush(););
    }
  }

  LLVM_DEBUG(errs() << "\n");
}

void GSAFTraceScorer::check_all() {
  // TODO: add all the scorers here

  // Domination Checking
  if (tactic & TACTIC_FLAGS::DOMINATION) {
    LLVM_DEBUG(dbgs() << "Start dominate_checking .....");
    dominate_checking();
    LLVM_DEBUG(dbgs() << "Done\n");
  }

  // Validity Checking
  if (tactic & TACTIC_FLAGS::VALIDITY) {
    LLVM_DEBUG(dbgs() << "Start path validity checking .....");
    path_validity_checking();
    LLVM_DEBUG(dbgs() << "Done\n");
  }

  // Confidence score checking
  if (tactic & TACTIC_FLAGS::CONFIDENCE_ALL) {
    LLVM_DEBUG(dbgs() << "Start confidence score checking .....");
    if (tactic & TACTIC_FLAGS::TRACE) {
      trace_confidence_checking();
    }

    if (tactic & TACTIC_FLAGS::PATH) {
      path_confidence_checking();
    }

    if (tactic & TACTIC_FLAGS::CONTEXT) {
      context_confidence_checking();
    }

    int report_size = report_items.size();
    for (int i = 0; i < report_size; i++) {
      report_items[i].compute_confidence();
    }
    LLVM_DEBUG(dbgs() << "Done\n");
  }

  int report_size = report_items.size();
  for (int i = 0; i < report_size; i++) {
    GuardedValueFlowTrace *trace = report_items[i].trace;
    if (trace) {
      int score = report_items[i].compute_score();
      trace->set_score(score);
    }
  }

  if (scorer_dump_score) {
    std::vector<int> sorted_index;
    sorted_index.reserve(report_size);
    for (int i = 0; i < report_size; i++) {
      sorted_index.push_back(i);
    }

    assert(report_size == sorted_index.size() &&
           "Incorrect sorting implementation");

    for (int i = 0; i < report_size; i++) {
      for (int j = report_size - 1; j > i; j--) {
        GuardedValueFlowTrace *trace_1 = report_items[sorted_index[j]].trace;
        GuardedValueFlowTrace *trace_2 =
            report_items[sorted_index[j - 1]].trace;

        if ((!trace_2) || (trace_1 != nullptr &&
                           trace_2->get_score() < trace_1->get_score())) {
          int tmp = sorted_index[j];
          sorted_index[j] = sorted_index[j - 1];
          sorted_index[j - 1] = tmp;
        }
      }
    }

    outs() << "\n";
    output_padded_text(outs(), "", 150, '*', true);
    for (int i = 0; i < report_size; i++) {
      GuardedValueFlowTrace *trace = report_items[sorted_index[i]].trace;
      if (trace && trace->getKeyIdx() != GuardedValueFlowTrace::IDX_INVALID) {
        outs() << "Scorer : (idx = " << sorted_index[i]
               << "\t, vul = " << (*trace)[trace->getKeyIdx()] << ") "
               << "|trace = "
               << format_str(CONFIDENCE_OUTPUT_FORMAT,
                             report_items[sorted_index[i]].trace_confidence)
               << "|path = "
               << format_str(CONFIDENCE_OUTPUT_FORMAT,
                             report_items[sorted_index[i]].path_confidence)
               << "|context = "
               << format_str(CONFIDENCE_OUTPUT_FORMAT,
                             report_items[sorted_index[i]].context_confidence)
               << "|constructive = "
               << format_str(CONFIDENCE_OUTPUT_FORMAT,
                             trace->get_constructive_confidence())
               << "|confidence = "
               << format_str(CONFIDENCE_OUTPUT_FORMAT,
                             report_items[sorted_index[i]].confidence)
               << "|dominated = " << report_items[sorted_index[i]].is_dominated
               << "|valid = " << report_items[sorted_index[i]].is_valid
               << "|score = " << trace->get_score() << "\n";

        if (const GuardedValueFlowObject *key_step =
                (*trace)[trace->getKeyIdx()]) {
          if (key_step) {
            if (Instruction *inst = key_step->getDebugInstruction()) {
              outs() << "[" << DIA->getSourceLocation(inst) << "]";
            } else {
              if (const GuardedValueFlowNode *seg_node =
                      dyn_cast<GuardedValueFlowNode>(key_step)) {
                outs() << seg_node->getDescription();
              } else {
                outs() << "**" << *key_step << "**";
              }
            }
          } else {
            outs() << "<NA>";
          }
          outs() << "\n";
        }
      }
    }
  }
}

int GSAFTraceScorer::get_index(GuardedValueFlowTrace *trace) {
  auto Res = report_index.find(trace);
  if (Res == report_index.end()) {
    // unknown report
    return GSAFTraceScorer::UNKNOWN;
  }
  int idx = Res->second;
  return idx;
}

bool GSAFTraceScorer::is_dominated(int item_idx) {
  int report_size = report_items.size();
  if (item_idx < 0 || item_idx >= report_size) {
    // we do not prune the unknown bug reports
    return false;
  }
  return report_items[item_idx].is_dominated;
}

bool GSAFTraceScorer::is_dominated(GuardedValueFlowTrace *trace) {
  int idx = get_index(trace);
  return is_dominated(idx);
}

bool GSAFTraceScorer::is_valid(int item_idx) {
  int report_size = report_items.size();
  if (item_idx < 0 || item_idx >= report_size) {
    // we do not prune the unknown bug reports
    return true;
  }
  return report_items[item_idx].is_valid;
}

bool GSAFTraceScorer::is_valid(GuardedValueFlowTrace *trace) {
  int idx = get_index(trace);
  return is_valid(idx);
}

float GSAFTraceScorer::get_confidence(int item_idx) {
  int report_size = report_items.size();
  if (item_idx < 0 || item_idx >= report_size) {
    // we do not prune the unknown bug reports
    return 1.0f;
  }
  return report_items[item_idx].confidence;
}

float GSAFTraceScorer::get_confidence(GuardedValueFlowTrace *trace) {
  int idx = get_index(trace);
  return get_confidence(idx);
}

GSAFClusteredTrace &GSAFTraceScorer::get_clustered_trace() {
  return clustered_trace;
}

void GSAFTraceScorer::dump_clustered_trace(raw_ostream &O) {
  clustered_trace.dump(O, true, DIA, this);
}

void GSAFTraceScorer::dump_clustered_trace_undomed(raw_ostream &O) {
  clustered_trace.dump(O, false, DIA, this);
}

GSAFTraceScorer::Iterator GSAFTraceScorer::all_begin() {
  // operator++ from -1 => the first index matching the condition
  Iterator result(-1, Iterator::iter_type::ALL_VALID, *this);
  result++;
  return result;
}

GSAFTraceScorer::Iterator GSAFTraceScorer::all_end() {
  int report_size = report_items.size();
  Iterator result(report_size, Iterator::iter_type::ALL_VALID, *this);
  return result;
}

GSAFTraceScorer::Iterator GSAFTraceScorer::nondom_begin() {
  Iterator result(-1, Iterator::iter_type::NON_DOM_ONLY, *this);
  result++;
  return result;
}

GSAFTraceScorer::Iterator GSAFTraceScorer::nondom_end() {
  int report_size = report_items.size();
  Iterator result(report_size, Iterator::iter_type::NON_DOM_ONLY, *this);
  return result;
}

GSAFTraceScorer::Iterator::Iterator(GSAFTraceScorer &pv) : parent(pv) {
  index = 0;
  type = ALL_VALID;
}

GSAFTraceScorer::Iterator::Iterator(int idx, iter_type t, GSAFTraceScorer &pv)
    : parent(pv) {
  index = idx;
  type = t;
}

GSAFTraceScorer::Iterator::Iterator(const Iterator &iter)
    : parent(iter.parent) {
  index = iter.index;
  type = iter.type;
}

void GSAFTraceScorer::Iterator::operator++(int) {
  int report_size = parent.report_items.size();

  while (++index < report_size) {
    if (parent.report_items[index].is_dominated == false &&
        type == NON_DOM_ONLY) {
      break;
    }
    if (type == ALL_VALID) {
      break;
    }
  }
}

bool GSAFTraceScorer::Iterator::operator==(const Iterator &iter) {
  return iter.index == index;
}

bool GSAFTraceScorer::Iterator::operator!=(const Iterator &iter) {
  return iter.index != index;
}

GuardedValueFlowTrace &GSAFTraceScorer::Iterator::operator*() {
  return *parent.report_items[index].trace;
}

GuardedValueFlowTrace *GSAFTraceScorer::Iterator::operator->() {
  return parent.report_items[index].trace;
}

/*
 * ================================ General Trace processing utilities for
 * domination checking ==============================
 */
// Return true if "from" is ordered before "to" in the same BB
static bool order_before_in_BB(Instruction *from, Instruction *to) {
  bool res = false;
  if (from->getParent() == to->getParent()) {
    BasicBlock *bb = from->getParent();
    // bool found_from = false;
    for (Instruction &inst : *bb) {
      if (&inst == to) {
        res = false;
        break;
      }
      if (&inst == from) {
        res = true;
        break;
      }
    }
  }
  return res;
}

// Return true if "from" is ordered before "to" in the same BB
static bool order_before_in_BB(const GuardedValueFlowObject *from,
                               const GuardedValueFlowObject *to) {
  bool res = false;
  if (from->getParentBasicBlock() == to->getParentBasicBlock()) {
    BasicBlock *bb = from->getParentBasicBlock();

    Instruction *from_inst = from->getDebugInstruction();
    Instruction *to_inst = to->getDebugInstruction();

    if (!from_inst) {
      // from instruction is null means that this is a special node such as
      // argument Such nodes are regarded as before all instructions similar to
      // to_inst
      return true;
    }

    if (!to_inst) {
      return false;
    }

    // bool found_from = false;
    for (Instruction &inst : *bb) {
      if (&inst == to_inst) {
        res = false;
        break;
      }
      if (&inst == from_inst) {
        res = true;
        break;
      }
    }
  }
  return res;
}

// return true if ins_from generates the value used by ins_to.
//  TODO: This function is not used now, may be removed after domination
//  checking is tested stable
// static bool is_dep(Instruction* ins_from, Instruction* ins_to) {
//	bool res = false;
//	for (unsigned int i = 0; i < ins_to->getNumOperands(); i++) {
//		if (ins_to->getOperand(i) == ins_from) {
//			res = true;
//			break;
//		}
//	}
//	return res;
// }

// Compute the set of reachable BBs from \p cur_bb
static void get_reachable_BBs(BasicBlock *cur_bb,
                              unordered_set<BasicBlock *> &visited) {
  visited.insert(cur_bb);
  Instruction *term = cur_bb->getTerminator();
  int n_successors = term->getNumSuccessors();

  for (int i = 0; i < n_successors; i++) {
    BasicBlock *child_bb = term->getSuccessor(i);
    if (!(visited.count(child_bb))) {
      get_reachable_BBs(child_bb, visited);
    }
  }
}

/*
 * Remove all bb1 from \p BBs if there is path from \p cur_bb -> bb1
 * without going through any bb2 in \p vuln_bbs.
 */
static void remove_undom_BBs(BasicBlock *cur_bb,
                             unordered_set<BasicBlock *> &BBs,
                             unordered_set<BasicBlock *> &vuln_bbs) {
  BBs.erase(cur_bb);

  if (vuln_bbs.count(cur_bb))
    return;

  Instruction *term = cur_bb->getTerminator();
  int n_successors = term->getNumSuccessors();
  for (int i = 0; i < n_successors; i++) {
    BasicBlock *child_bb = term->getSuccessor(i);
    if (BBs.count(child_bb)) {
      remove_undom_BBs(child_bb, BBs, vuln_bbs);
    }
  }
}

// Report B is dominated by Report A if happening of B implies the happening of
// A, i.e. A must happen before B Our aim here is finding and marking all the
// non-dominated traces This is the old version of domination checking,
//      which is not currently used because some SPEG nodes do not have
//      llvm::instruction/llvm::value info
void GSAFTraceScorer::do_dominate_checking_on_value() {
  // evf_targets_for[x] is the set of EVF targets y for instruction x,
  // where <x, y> is a last EVF of certain trace
  // EVF stands for effective value flow, see the comments for function
  // get_last_EVF for details
  unordered_map<Instruction *, unordered_set<Instruction *>> levf_targets_for;

  // dominated_bbs_by[x] is the set of BBs that are dominated by vulnerable
  // instruction x
  unordered_map<Instruction *, unordered_set<BasicBlock *>> dominated_bbs_by;

  // ins_used_in[x] is the set of BBs that uses x (x is an instruction in our
  // setting)
  unordered_map<Instruction *, unordered_set<BasicBlock *>> ins_used_in;

  // The set of last EVF pointers that are defined and used in the same BB
  // the int value is the index of the trace with the earliest triggering of the
  // vulnerability
  unordered_map<Instruction *, int> inplace_used_levf_ptrs;

  // Initialize the cache of the traces, classifications,
  //      the instructions that pass the value to a vulnerability point,
  //      together with the vulnerability that are used in the same BB
  int report_size = report_items.size();
  for (int i = 0; i < report_size; i++) {
    GuardedValueFlowTrace *trace = report_items[i].trace;
    if ((!trace) || trace->get_length() == 0)
      continue;

    // Get the last effective value flow (LEVF) for i^th trace
    int LEVF_from_idx = trace->getLEVFSrcIdx();
    int LEVF_to_idx = trace->getKeyIdx();

    if (LEVF_from_idx == GuardedValueFlowTrace::IDX_INVALID ||
        LEVF_to_idx == GuardedValueFlowTrace::IDX_INVALID) {
      continue;
    }

    const GuardedValueFlowObject *LEVF_from_node = (*trace)[LEVF_from_idx];
    const GuardedValueFlowObject *LEVF_to_node = (*trace)[LEVF_to_idx];

    Instruction *LEVF_from_inst =
        LEVF_from_node ? LEVF_from_node->getDebugInstruction() : nullptr;
    Instruction *LEVF_to_inst =
        LEVF_to_node ? LEVF_to_node->getDebugInstruction() : nullptr;

    if (LEVF_from_inst == nullptr || LEVF_to_inst == nullptr) {
      LLVM_DEBUG(errs() << DEBUG_TYPE << ": "
                        << "Illy formed last EVF in Trace %d" << i << "\n";);
    } else {
      // Record last EVF
      levf_targets_for[LEVF_from_inst].insert(LEVF_to_inst);

      /*
       * We collect the levf_from pointers that are:
       * 1. Defined and used as vulnerability site in the same block;
       * 2. Used directly by the vulnerability sites (i.e. there is no GEP or
       * bitcast instructions in between).
       */
      if (order_before_in_BB(LEVF_from_inst, LEVF_to_inst)) {
        if (!inplace_used_levf_ptrs.count(LEVF_from_inst)) {
          inplace_used_levf_ptrs[LEVF_from_inst] = i;
        } else {
          int cur_dom_idx = inplace_used_levf_ptrs[LEVF_from_inst];
          assert(0 <= cur_dom_idx && cur_dom_idx < report_size &&
                 "index error for domination computation");

          GuardedValueFlowTrace *dom_trace = report_items[cur_dom_idx].trace;
          if (dom_trace &&
              dom_trace->getKeyIdx() != GuardedValueFlowTrace::IDX_INVALID) {
            const GuardedValueFlowObject *dom_node =
                (*dom_trace)[dom_trace->getKeyIdx()];
            Instruction *cur_dom_levf_to =
                dom_node ? dom_node->getDebugInstruction() : nullptr;
            if (cur_dom_levf_to) {
              if (order_before_in_BB(LEVF_to_inst, cur_dom_levf_to)) {
                inplace_used_levf_ptrs[LEVF_from_inst] = i;
              }
            } else {
              // Defensive programming
              // This should be dead code
              inplace_used_levf_ptrs[LEVF_from_inst] = i;
            }
          } else {
            // Defensive programming
            // This should be dead code
            inplace_used_levf_ptrs[LEVF_from_inst] = i;
          }
        }
      }
    }
  }

  // Compute the BBs that are dominated by each vulnerability site
  for (int i = 0; i < report_size; i++) {
    GuardedValueFlowTrace *trace = report_items[i].trace;
    if ((!trace) || trace->get_length() == 0)
      continue;

    // Get the last effective value flow (LEVF) for i^th trace
    int LEVF_from_idx = trace->getLEVFSrcIdx();
    int LEVF_to_idx = trace->getKeyIdx();

    if (LEVF_from_idx == GuardedValueFlowTrace::IDX_INVALID ||
        LEVF_to_idx == GuardedValueFlowTrace::IDX_INVALID) {
      continue;
    }

    const GuardedValueFlowObject *LEVF_from_node = (*trace)[LEVF_from_idx];
    const GuardedValueFlowObject *LEVF_to_node = (*trace)[LEVF_to_idx];

    Instruction *LEVF_from_inst =
        LEVF_from_node ? LEVF_from_node->getDebugInstruction() : nullptr;
    Instruction *LEVF_to_inst =
        LEVF_to_node ? LEVF_to_node->getDebugInstruction() : nullptr;

    if (LEVF_from_inst == nullptr || LEVF_to_inst == nullptr ||
        // If safe_inst contains ins_from, all the value passes are dominated
        inplace_used_levf_ptrs.count(LEVF_from_inst) ||
        // We have already processed this instruction
        dominated_bbs_by.find(LEVF_from_inst) != dominated_bbs_by.end() ||
        // From-idx >= to-idx means that the trace is not for domination
        // checking Thus, we apply no domination checking
        LEVF_from_idx >= LEVF_to_idx)
      continue;

    unordered_set<BasicBlock *> &dom_bbs = dominated_bbs_by[LEVF_from_inst];
    unordered_set<BasicBlock *> &use_bbs = ins_used_in[LEVF_from_inst];
    unordered_set<Instruction *> &levf_targets =
        levf_targets_for[LEVF_from_inst];

    /*
     * Following code computes the set of reachable BBs from evf_from, where
     * each BB in the set is dominated by at least one of the instruction in
     * evf_targets.
     */
    Function *from_func = ir_expression::getEnclosingFunction(LEVF_from_inst);
    assert(from_func != nullptr &&
           "A non-null instruction must be in a function. Something wrong?");

    // First get the set of BBs and enclosing functions for the vulnerability
    // sites in levf_targets
    unordered_set<Function *> to_funcs;
    for (Instruction *inst : levf_targets) {
      use_bbs.insert(inst->getParent());
      to_funcs.insert(inst->getParent()->getParent());
    }

    // Second we compute the set of BBs that are reachable from levf_from
    // without going through any BBs in use_bbs (the bbs containing the to_value
    // to the vulnerability point)
    for (Function *func : to_funcs) {
      BasicBlock *start_bb = (from_func == func) ? LEVF_from_inst->getParent()
                                                 : &func->getEntryBlock();
      get_reachable_BBs(start_bb, dom_bbs);
      remove_undom_BBs(start_bb, dom_bbs, use_bbs);
    }
  }

  // Check whether a report is dominated by other reports
  for (int i = 0; i < report_size; i++) {
    GuardedValueFlowTrace *trace = report_items[i].trace;
    if ((!trace) || trace->get_length() == 0)
      continue;

    // Get the last effective value flow (LEVF) for i^th trace
    int LEVF_from_idx = trace->getLEVFSrcIdx();
    int LEVF_to_idx = trace->getKeyIdx();

    if (LEVF_from_idx == GuardedValueFlowTrace::IDX_INVALID ||
        LEVF_to_idx == GuardedValueFlowTrace::IDX_INVALID) {
      continue;
    }

    if (LEVF_to_idx < LEVF_from_idx)
      continue;

    int cur_step = trace->getValidStartIdx();
    Instruction *EVF_from;
    Instruction *EVF_to = (*trace)[cur_step]->getDebugInstruction();
    while (cur_step < LEVF_from_idx) {
      cur_step++;
      const GuardedValueFlowObject *step_node = (*trace)[cur_step];
      if (!step_node) {
        continue;
      }
      EVF_to = step_node->getDebugInstruction();
      if (EVF_to) {
        break;
      }
    }

    while (cur_step < LEVF_from_idx) {
      EVF_from = EVF_to;
      while (cur_step < LEVF_from_idx) {
        cur_step++;
        const GuardedValueFlowObject *step_node = (*trace)[cur_step];
        if (!step_node) {
          continue;
        }
        EVF_to = step_node->getDebugInstruction();
        if (EVF_to) {
          break;
        }
      }

      if (EVF_from == nullptr || EVF_to == nullptr) {
        continue;
      }

      if (inplace_used_levf_ptrs.count(EVF_from) &&
          inplace_used_levf_ptrs[EVF_from] != i) {
        // Another trace that also uses evf_from in the same BB must dominate
        // trace[i]
        report_items[i].is_dominated = true;
        break;
      }

      // We check if the basicblock is dominated by vulnerability sites
      if (dominated_bbs_by.count(EVF_from)) {
        unordered_set<BasicBlock *> &dom_bb = dominated_bbs_by[EVF_from];
        unordered_set<BasicBlock *> &use_bb = ins_used_in[EVF_from];
        if (dom_bb.count(EVF_to->getParent()) ||
            use_bb.count(EVF_to->getParent())) {
          report_items[i].is_dominated = true;
          break;
        }
      }

      if (report_items[i].is_dominated) {
        break;
      }
    }

    // Handling last step, if we cannot confirm the trace is dominated according
    // to the previous steps
    if (!report_items[i].is_dominated && LEVF_from_idx < LEVF_to_idx) {
      const GuardedValueFlowObject *LEVF_from = (*trace)[LEVF_from_idx];
      const GuardedValueFlowObject *LEVF_to = (*trace)[LEVF_to_idx];

      if (LEVF_from != nullptr && LEVF_to != nullptr) {
        Instruction *LEVF_from_inst =
            LEVF_from ? LEVF_from->getDebugInstruction() : nullptr;
        Instruction *LEVF_to_inst =
            LEVF_to ? LEVF_to->getDebugInstruction() : nullptr;

        if (LEVF_from_inst != nullptr && LEVF_to_inst != nullptr) {
          if (inplace_used_levf_ptrs.count(LEVF_from_inst) &&
              inplace_used_levf_ptrs[LEVF_from_inst] != i) {
            // Another trace that also uses evf_from in the same BB must
            // dominate trace[i]
            report_items[i].is_dominated = true;
          }

          if ((!report_items[i].is_dominated) &&
              dominated_bbs_by.count(LEVF_from_inst)) {
            // Check inter-BB dominance
            unordered_set<BasicBlock *> &dom_bb =
                dominated_bbs_by[LEVF_from_inst];
            if (dom_bb.count(LEVF_to_inst->getParent())) {
              report_items[i].is_dominated = true;
            }
          }

          if ((!report_items[i].is_dominated) &&
              levf_targets_for.count(LEVF_from_inst)) {
            // Check intra-BB dominance
            unordered_set<Instruction *> &dom_inst =
                levf_targets_for[LEVF_from_inst];
            for (Instruction *inst : dom_inst) {
              if (inst != LEVF_to_inst) {
                if (order_before_in_BB(inst, LEVF_to_inst)) {
                  report_items[i].is_dominated = true;
                  break;
                }
              }
            }
          }
        }
      }
    }
  }
}

// Report B is dominated by Report A if happening of B implies the happening of
// A, i.e. A must happen before B Our aim here is finding and marking all the
// non-dominated traces
void GSAFTraceScorer::do_dominate_checking_on_gvfg_node() {
  // evf_targets_for[x] is the set of EVF targets y for GVFG node x,
  // where <x, y> is a last EVF of certain trace
  // EVF stands for effective value flow, see the comments for function
  // get_last_EVF for details
  unordered_map<const GuardedValueFlowObject *,
                unordered_set<const GuardedValueFlowObject *>>
      levf_targets_for;

  // dominated_bbs_by[x] is the set of BBs that are dominated by vulnerable GVFG
  // Node x
  unordered_map<const GuardedValueFlowObject *, unordered_set<BasicBlock *>>
      dominated_bbs_by;

  // ins_used_in[x] is the set of BBs that uses x (x is an GVFG node in our
  // setting)
  unordered_map<const GuardedValueFlowObject *, unordered_set<BasicBlock *>>
      ins_used_in;

  // The set of last EVF pointers that are defined and used in the same BB
  // the int value is the index of the trace with the earliest triggering of the
  // vulnerability
  unordered_map<const GuardedValueFlowObject *, const GuardedValueFlowObject *>
      inplace_used_levf_ptrs;

  // Initialize the cache of the traces, classifications,
  //      the GVFG nodes that pass the value to a vulnerability point,
  //      together with the vulnerability that are used in the same BB
  int report_size = report_items.size();
  for (int i = 0; i < report_size; i++) {
    GuardedValueFlowTrace *trace = report_items[i].trace;
    if ((!trace) || trace->get_length() == 0)
      continue;

    std::vector<int> LEVF_from_indices;

    // Get the last effective value flow (LEVF) for i^th trace
    int LEVF_from_idx_ignore_load = trace->getLEVFSrcIdx(false);
    int LEVF_from_idx_consider_load = trace->getLEVFSrcIdx(true);
    int LEVF_to_idx = trace->getKeyIdx();

    if (LEVF_to_idx == GuardedValueFlowTrace::IDX_INVALID) {
      continue;
    }

    // Since IR shall either use a load-store pair to get the value
    //       or apply an optimization by directly using the previously loaded
    //       value,
    // here, we have to consider both possibilities.
    if (LEVF_from_idx_ignore_load != GuardedValueFlowTrace::IDX_INVALID)
      LEVF_from_indices.push_back(LEVF_from_idx_ignore_load);
    if (LEVF_from_idx_consider_load != LEVF_from_idx_ignore_load &&
        LEVF_from_idx_consider_load != GuardedValueFlowTrace::IDX_INVALID)
      LEVF_from_indices.push_back(LEVF_from_idx_consider_load);

    for (int LEVF_from_idx : LEVF_from_indices) {
      const GuardedValueFlowObject *LEVF_from_node = (*trace)[LEVF_from_idx];
      const GuardedValueFlowObject *LEVF_to_node = (*trace)[LEVF_to_idx];

      if (LEVF_from_node == nullptr || LEVF_to_node == nullptr) {
        LLVM_DEBUG(errs() << DEBUG_TYPE << ": "
                          << "Illy formed last EVF in Trace %d" << i << "\n";);
      } else if (LEVF_from_idx < LEVF_to_idx) {
        // Record last EVF
        levf_targets_for[LEVF_from_node].insert(LEVF_to_node);

        // We collect the levf_from pointers that are defined and used as
        // vulnerability site in the same block
        if (order_before_in_BB(LEVF_from_node, LEVF_to_node)) {
          if (!inplace_used_levf_ptrs.count(LEVF_from_node)) {
            inplace_used_levf_ptrs[LEVF_from_node] = LEVF_to_node;
          } else {
            const GuardedValueFlowObject *dom_node =
                inplace_used_levf_ptrs[LEVF_from_node];
            if (dom_node) {
              if (order_before_in_BB(LEVF_to_node, dom_node)) {
                inplace_used_levf_ptrs[LEVF_from_node] = LEVF_to_node;
              }
            } else {
              // Defensive programming
              // This should be dead code
              inplace_used_levf_ptrs[LEVF_from_node] = LEVF_to_node;
            }
          }
        }
      }
    }
  }

  // Compute the BBs that are dominated by each vulnerability site
  for (int i = 0; i < report_size; i++) {
    GuardedValueFlowTrace *trace = report_items[i].trace;
    if ((!trace) || trace->get_length() == 0 ||
        trace->getKeyIdx() != GuardedValueFlowTrace::IDX_INVALID)
      continue;

    std::vector<int> LEVF_from_indices;

    // Get the last effective value flow (LEVF) for i^th trace
    int LEVF_from_idx_ignore_load = trace->getLEVFSrcIdx(false);
    int LEVF_from_idx_consider_load = trace->getLEVFSrcIdx(true);
    int LEVF_to_idx = trace->getKeyIdx();

    if (LEVF_to_idx == GuardedValueFlowTrace::IDX_INVALID) {
      continue;
    }

    // Since IR shall either use a load-store pair to get the value
    //       or apply an optimization by directly using the previously loaded
    //       value,
    // here, we have to consider both possibilities.
    if (LEVF_from_idx_ignore_load != GuardedValueFlowTrace::IDX_INVALID)
      LEVF_from_indices.push_back(LEVF_from_idx_ignore_load);
    if (LEVF_from_idx_consider_load != LEVF_from_idx_ignore_load &&
        LEVF_from_idx_consider_load != GuardedValueFlowTrace::IDX_INVALID)
      LEVF_from_indices.push_back(LEVF_from_idx_consider_load);

    for (int LEVF_from_idx : LEVF_from_indices) {
      const GuardedValueFlowObject *LEVF_from_node = (*trace)[LEVF_from_idx];
      const GuardedValueFlowObject *LEVF_to_node = (*trace)[LEVF_to_idx];

      if (LEVF_from_node == nullptr || LEVF_to_node == nullptr ||
          // If safe_inst contains ins_form, all the value passes are dominated
          inplace_used_levf_ptrs.count(LEVF_from_node) ||
          // We have already processed this GVFG node
          dominated_bbs_by.find(LEVF_from_node) != dominated_bbs_by.end() ||
          // From-idx >= to-idx means that the trace is not for domination
          // checking Thus, we apply no domination checking
          LEVF_from_idx >= LEVF_to_idx)
        continue;

      unordered_set<BasicBlock *> &dom_bbs = dominated_bbs_by[LEVF_from_node];
      unordered_set<BasicBlock *> &use_bbs = ins_used_in[LEVF_from_node];
      unordered_set<const GuardedValueFlowObject *> &levf_targets =
          levf_targets_for[LEVF_from_node];

      /*
       * Following code computes the set of reachable BBs from evf_from, where
       * each BB in the set is dominated by at least one of the GVFG node in
       * evf_targets.
       */
      Function *from_func = LEVF_from_node->getParentBasicBlock()->getParent();
      assert(from_func != nullptr &&
             "Each GVFG node should be in a function. Something wrong?");

      // First get the set of BBs and enclosing functions for the vulnerability
      // sites in levf_targets
      unordered_set<Function *> to_funcs;
      for (const GuardedValueFlowObject *seg_node : levf_targets) {
        use_bbs.insert(seg_node->getParentBasicBlock());
        to_funcs.insert(seg_node->getParentBasicBlock()->getParent());
      }

      // Second we compute the set of BBs that are reachable from levf_from
      // without going through any BBs in use_bbs (the bbs containing the
      // to_value to the vulnerability point)
      for (Function *func : to_funcs) {
        BasicBlock *start_bb = (from_func == func)
                                   ? LEVF_from_node->getParentBasicBlock()
                                   : &func->getEntryBlock();
        get_reachable_BBs(start_bb, dom_bbs);
        remove_undom_BBs(start_bb, dom_bbs, use_bbs);
      }
    }
  }

  // Check whether a report is dominated by other reports
  for (int i = 0; i < report_size; i++) {
    GuardedValueFlowTrace *trace = report_items[i].trace;
    if ((!trace) || trace->get_length() == 0)
      continue;

    // Get the last effective value flow (LEVF) for i^th trace
    int LEVF_from_idx = trace->getLEVFSrcIdx();
    int LEVF_to_idx = trace->getKeyIdx();

    if (LEVF_from_idx == GuardedValueFlowTrace::IDX_INVALID ||
        LEVF_to_idx == GuardedValueFlowTrace::IDX_INVALID)
      continue;

    if (LEVF_to_idx <= LEVF_from_idx)
      continue;

    int cur_step = trace->getValidStartIdx();
    const GuardedValueFlowObject *EVF_from;
    const GuardedValueFlowObject *EVF_to = (*trace)[cur_step];
    while (cur_step < LEVF_from_idx) {
      cur_step++;
      EVF_to = (*trace)[cur_step];
      if (!EVF_to) {
        continue;
      }

      Instruction *EVF_to_inst = EVF_to->getDebugInstruction();
      Value *EVF_to_val = EVF_to->getDebugValue();
      if (EVF_to_inst != nullptr ||
          (EVF_to_val != nullptr && isa<Argument>(EVF_to_val))) {
        // We only consider value flows among instructions and arguments in the
        // trace
        break;
      }
    }

    while (cur_step < LEVF_from_idx) {
      EVF_from = EVF_to;
      while (cur_step < LEVF_from_idx) {
        cur_step++;

        EVF_to = (*trace)[cur_step];
        if (!EVF_to) {
          continue;
        }

        Instruction *EVF_to_inst = EVF_to->getDebugInstruction();
        Value *EVF_to_val = EVF_to->getDebugValue();
        if (EVF_to_inst != nullptr ||
            (EVF_to_val != nullptr && isa<Argument>(EVF_to_val))) {
          // We only consider value flows among instructions and arguments in
          // the trace
          break;
        }
      }

      if (EVF_from == nullptr || EVF_to == nullptr) {
        continue;
      }

      if (inplace_used_levf_ptrs.count(EVF_from) &&
          inplace_used_levf_ptrs[EVF_from] != (*trace)[trace->getKeyIdx()]) {
        // Another trace that also uses evf_from in the same BB must dominate
        // trace[i]
        report_items[i].is_dominated = true;
        break;
      }

      // We check if the basicblock is dominated by vulnerability sites
      if (dominated_bbs_by.count(EVF_from)) {
        unordered_set<BasicBlock *> &dom_bb = dominated_bbs_by[EVF_from];
        unordered_set<BasicBlock *> &use_bb = ins_used_in[EVF_from];
        if (dom_bb.count(EVF_to->getParentBasicBlock()) ||
            use_bb.count(EVF_to->getParentBasicBlock())) {
          report_items[i].is_dominated = true;
          break;
        }
      }

      if (report_items[i].is_dominated) {
        break;
      }
    }

    // Handling Last step, if we cannot confirm the trace is dominated according
    // to the previous steps
    if (!report_items[i].is_dominated) {
      const GuardedValueFlowObject *LEVF_from = (*trace)[LEVF_from_idx];
      const GuardedValueFlowObject *LEVF_to = (*trace)[LEVF_to_idx];

      if (LEVF_from != nullptr && LEVF_to != nullptr) {
        if (inplace_used_levf_ptrs.count(LEVF_from) &&
            inplace_used_levf_ptrs[LEVF_from] != LEVF_to) {
          // Another trace that also uses evf_from in the same BB must dominate
          // trace[i]
          report_items[i].is_dominated = true;
        }

        if ((!report_items[i].is_dominated) &&
            dominated_bbs_by.count(LEVF_from)) {
          // Check inter-BB dominance
          unordered_set<BasicBlock *> &dom_bb = dominated_bbs_by[LEVF_from];
          if (dom_bb.count(LEVF_to->getParentBasicBlock())) {
            report_items[i].is_dominated = true;
          }
        }

        if ((!report_items[i].is_dominated) &&
            levf_targets_for.count(LEVF_from)) {
          // Check intra-BB dominance
          unordered_set<const GuardedValueFlowObject *> &dom_nodes =
              levf_targets_for[LEVF_from];
          for (const GuardedValueFlowObject *seg_node : dom_nodes) {
            if (seg_node && seg_node != LEVF_to) {
              if (order_before_in_BB(seg_node, LEVF_to)) {
                report_items[i].is_dominated = true;
                break;
              }
            }
          }
        }
      }
    }
  }
}
} // namespace lotus::gsaf
