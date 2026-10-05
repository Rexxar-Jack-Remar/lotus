#include "Checker/GSAF/Engine/FunctionAnalyzer.h"

#include "Checker/GSAF/API/Vulnerability.h"
#include "Checker/GSAF/Support/GraphQueries.h"
#include "Checker/GSAF/Support/Options.h"
#include "Utils/Parallel/ThreadPool.h"

#include <list>
#include <utility>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
namespace gsaf {
using namespace llvm;

/*==----macros for debugging----==*/

#define DEBUG_TYPE "gsaf-function-analyzer"

#define TAINT_WRAPPER_STR "\033[0;31m*Taint wrapper: \033[0m"
#define BUG_STR "\033[0;31m*Bug: \033[0m"
#define SKIP_STR "\033[0;32m#Skip: \033[0m"
#define OUT_SUMMARY_SEP_START                                                  \
  "\033[0;34m/*==----------Out-Summary Start--------==*/\033[0m"
#define OUT_SUMMARY_SEP_END                                                    \
  "\033[0;34m/*==-----------Out-Summary End---------==*/\033[0m"

#define DEBUG_CONSTRAINTS(X)                                                   \
  do {                                                                         \
    if (GSAFOptions::DebugConstraints && F->hasName() &&                       \
        F->getName() == GSAFOptions::DebugFunction) {                          \
      X;                                                                       \
    }                                                                          \
  } while (0)

#define DEBUG_TRACE(X)                                                         \
  do {                                                                         \
    if (GSAFOptions::DebugTrace && F->hasName() &&                             \
        F->getName() == GSAFOptions::DebugFunction) {                          \
      X;                                                                       \
    }                                                                          \
  } while (0)

#define DEBUG_Graph(X)                                                         \
  do {                                                                         \
    if (GSAFOptions::DotGVFGValFlow && F->hasName() &&                         \
        F->getName() == GSAFOptions::DebugFunction) {                          \
      X;                                                                       \
    }                                                                          \
  } while (0)

FunctionAnalyzer::FunctionAnalyzer(GSAFChecker *P, Function *F)
    : GSAFFunctionWorker(P, F), BugTraces(P->FuncTraceMap[F]),
      Smry(P->FuncSmryMap[F]), DT(&P->getDomTree(F)),
      PDT(&P->getPostDomTree(F)), Graph(P->getGraph(F)) {
  TimeChecker = new Timer(
      GSAFOptions::Timeout,
      [F]() { throw std::runtime_error("[TIMEOUT] " + F->getName().str()); },
      6);

  SMTTimeChecker = new Timer(
      GSAFOptions::Timeout,
      [F]() {
        throw std::runtime_error("[SMT TIMEOUT] " + F->getName().str());
      },
      1);
  SMTTimeChecker->suspend();

  Fctry = new SMTFactory;

  if (GSAFOptions::SolverVersion == 1) {
    auto *configured = new GSAFSolver(*Fctry, P->getModule()->getDataLayout());
    configured->setModels(P->Models, GSAFOptions::EnableHeapAllocFailure,
                          GSAFOptions::EnableFileAllocFailure);
    Solver = configured;
  } else if (GSAFOptions::SolverVersion == 2) {
    auto *configured =
        new DTGSAFSolver(*Fctry, P->getModule()->getDataLayout(), DT);
    configured->setModels(P->Models, GSAFOptions::EnableHeapAllocFailure,
                          GSAFOptions::EnableFileAllocFailure);
    Solver = configured;
  } else {
    llvm_unreachable(
        "Unsupported symbolic expression solver (can be 1 and 2).");
  }

  initSummary();

  // TSV is the only vulnerability
  TSV = std::static_pointer_cast<TaintStyleVulnerability>(
      Parent->Vuln->getVulnerability(1));
}

FunctionAnalyzer::~FunctionAnalyzer() {
  delete Solver;
  delete TimeChecker;
  delete SMTTimeChecker;
  delete Fctry;
}

void FunctionAnalyzer::pushState() {
  NonSymDepsCache.push();
  SymbDepsCache.push();
  CallSiteOutputCache.push();
}

void FunctionAnalyzer::resetState() {
  NonSymDepsCache.reset();
  SymbDepsCache.reset();
  CallSiteOutputCache.reset();
}

void FunctionAnalyzer::popState() {
  NonSymDepsCache.pop();
  SymbDepsCache.pop();
  CallSiteOutputCache.pop();
}

InputSummary *FunctionAnalyzer::createInputSummary(
    const SMTExprVec &Constraints, std::shared_ptr<VulnerabilityTrace> Trace,
    const std::unordered_set<const GuardedValueFlowNode *> *Inputs,
    unsigned InlineDepth) {
  InputSummary *InSmry = new InputSummary(F, std::move(Trace), InlineDepth);
  InSmry->setVulnerabilityMask(-1);
  auto NonSymDep = Constraints.toAndExpr();
  InSmry->addNonSymDeps(SummaryCacheItem(&NonSymDep, "", 0));
  if (Inputs)
    InSmry->setInputs(*Inputs);

  return InSmry;
}

OutputSummary *FunctionAnalyzer::createOutputSummary(
    const SMTExprVec &Constraints, std::shared_ptr<VulnerabilityTrace> Trace,
    const std::unordered_set<const GuardedValueFlowNode *> *Inputs,
    unsigned InlineDepth) {
  OutputSummary *OutSmry = new OutputSummary(F, std::move(Trace), InlineDepth);
  OutSmry->setVulnerabilityMask(-1);
  auto NonSymDep = Constraints.toAndExpr();
  OutSmry->addNonSymDeps(SummaryCacheItem(&NonSymDep, "", 0));
  if (Inputs)
    OutSmry->setInputs(*Inputs);

  return OutSmry;
}

void FunctionAnalyzer::addCachedConstraintsToSummary(SummaryBase *S) {
  for (const SummaryCacheItem &item : NonSymDepsCache.getCacheVector()) {
    if ((unsigned)item.depth < GSAFOptions::InlineDepth)
      S->addNonSymDeps(item);
  }

  for (const SummaryCacheItem &item : SymbDepsCache.getCacheVector()) {
    if ((unsigned)item.depth < GSAFOptions::InlineDepth)
      S->addSymbDeps(item);
  }
}

// Main entry point for taint analysis.
// 1. Identifies taint sources.
// 2. Performs forward search from sources to identify reachable sinks or
// propagators.
// 3. Performs search from "Taint Source Wrappers" (special sources).
// 4. Performs search from arguments (potential external sources).
// 5. Performs search from call site outputs (sources from callees).
void FunctionAnalyzer::run() {
  DEBUG_WITH_TYPE("smt-solver",
                  dbgs() << "\nFunction Name: " << F->getName() << "\n");

  std::vector<Vulnerability::ValueSitePairType> SrcVec;
  TSV->setSources(Graph, SrcVec);

  for (auto &SrcIt : SrcVec) {
    const auto *SrcNode = SrcIt.first;
    const auto *SrcSite = SrcIt.second;

    Sources[SrcSite].insert(SrcNode);
  }

  DEBUG_Graph(
      std::vector<const GuardedValueFlowNode *> Srcs;
      std::for_each(SrcVec.begin(), SrcVec.end(),
                    [&Srcs](const Vulnerability::ValueSitePairType &Src) {
                      Srcs.push_back(Src.first);
                    });
      gvfg::GuardedValueFlowSerializer::writeDot(
          *Graph, (F->getName().str() + "-gsaf-value-flow.dot").c_str()););

  DEBUG_TRACE(dbgs() << "\t [Search from vulnerability-specified sources]\n");
  for (auto &SrcIt : SrcVec) {
    const auto *SrcNode = SrcIt.first;
    const auto *SrcSite = SrcIt.second;

    assert(SrcNode && SrcSite);

    DEBUG_TRACE(
        dbgs() << "\t [Search from vulnerability-specified sources (node) -> "
               << *SrcNode << "]\n");
    DEBUG_TRACE(
        dbgs() << "\t [Search from vulnerability-specified sources (site) -> "
               << *SrcSite << "]\n");

    if (TSV->checkNode(SrcNode, TraceBuilder)) {
      DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *SrcNode << "...\n");
      continue;
    }

    pushState();
    TraceBuilder.push();
    Solver->push();
    // the init constraints of the node
    // for npd, this constraints is true.
    Solver->addAll(Solver->getDataDeps(SrcNode));
    Solver->addAll(Solver->getCtrlDeps(SrcNode));

    TraceBuilder.add(SrcNode);
    if (SrcNode->getParentBasicBlock() != SrcSite->getParentBasicBlock())
      Solver->addAll(
          Solver->getCtrlDeps(SrcSite->getParentBasicBlock(), Graph));

    TraceBuilder.add(SrcSite);

    search(SrcNode, nullptr, std::make_pair(SrcNode, SrcSite), 0);

    Solver->pop();
    TraceBuilder.pop();
    popState();
  }

  // search from TaintInputSmryMap
  DEBUG_TRACE(dbgs() << "\t [Search from taint-function wrapper]\n");
  for (auto &SrcSiteIt : SourceWrapperSmryMap) {
    auto *SrcSite = SrcSiteIt.first;

    for (auto &SrcNoteIt : SrcSiteIt.second) {
      auto *SrcNode = dyn_cast<GuardedValueFlowNode>(SrcNoteIt.first);

      assert(SrcNode && SrcSite);

      DEBUG_TRACE(dbgs() << "\t [Search from taint-function wrapper (node) -> "
                         << *SrcNode << "]\n");
      DEBUG_TRACE(dbgs() << "\t [Search from taint-function wrapper (site) -> "
                         << *SrcSite << "]\n");

      if (TSV->checkNode(SrcNode, TraceBuilder)) {
        DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *SrcNode << "...\n");
        continue;
      }

      pushState();
      TraceBuilder.push();
      Solver->push();
      // the init constraints of the node
      Solver->addAll(Solver->getDataDeps(SrcNode));
      Solver->addAll(Solver->getCtrlDeps(SrcNode));
      if (SrcNode->getParentBasicBlock() != SrcSite->getParentBasicBlock())
        Solver->addAll(
            Solver->getCtrlDeps(SrcSite->getParentBasicBlock(), Graph));

      TraceBuilder.add(SrcNode);

      auto &SmrySet = SrcNoteIt.second;
      for (InputSummary *Smry : SmrySet) {
        inlineCalleeInSummary(SrcSite, Smry, std::make_pair(SrcNode, SrcSite),
                              (const GuardedValueFlowNode *)SrcNode, 0, 1);
      }

      Solver->pop();
      TraceBuilder.pop();
      popState();
    }
  }

  // search from arguments
  DEBUG_TRACE(dbgs() << "\t [Search from arguments]\n");
  for (auto ArgIt = Graph->arg_begin(), E = Graph->arg_end(); ArgIt != E;
       ++ArgIt) {
    auto *ArgNode = *ArgIt;

    if (!GSAFOptions::EnableSideEffectSource &&
        nodeOfKind<GuardedValueFlowNode::Kind::PseudoArgument,
                   GuardedValueFlowNode>(ArgNode)) {
      continue;
    }

    DEBUG_TRACE(dbgs() << "\t [Search from arguments -> " << *ArgNode << "]\n");

    if (TSV->checkNode(ArgNode, TraceBuilder)) {
      DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *ArgNode << "...\n");
      continue;
    }

    pushState();
    TraceBuilder.push();
    Solver->push();

    TraceBuilder.add(ArgNode);
    TraceBuilder.add(nullptr);

    bottomUpDepthFirstSearch(ArgNode, nullptr, std::make_pair(ArgNode, nullptr),
                             0);

    Solver->pop();
    TraceBuilder.pop();
    popState();
  }

  // search from call site outputs
  DEBUG_TRACE(dbgs() << "\t [Search from SourceOutSmryVec]\n");
  for (auto &SrcOutSmryIt : SourceOutSmryVec) {
    auto *CallSiteOutput = SrcOutSmryIt.first;
    auto *OutSmry = SrcOutSmryIt.second;
    assert(!isa<GuardedValueFlowNode>(OutSmry->getSourceNode()));

    DEBUG_TRACE(dbgs() << "\t [Search from SourceOutSmryVec -> "
                       << *CallSiteOutput << "]\n");

    auto Src = std::make_pair(CallSiteOutput, callSite(CallSiteOutput));
    inlineCalleeOutSummary(callSite(CallSiteOutput), OutSmry, CallSiteOutput,
                           Src, OutSmry->getInlineDepth() + 1);
  }

  buildReturnSymbolicSummary();

  finalizeSummary();
}

template <class T>
static void finalizeSummaryItem(
    std::map<const GuardedValueFlowNode *, std::set<T *, trace_summary_cmp>,
             ObjectLess> &SI) {
  if (GSAFOptions::MaxSummary == -1) {
    // no restriction
    return;
  }

  const int CARED_INLINE_DEPTH = 10;

  std::map<const GuardedValueFlowNode *, std::set<T *, trace_summary_cmp>,
           ObjectLess>
      TmpSmry[CARED_INLINE_DEPTH + 1];
  int Size[CARED_INLINE_DEPTH + 1] = {0};

  // partition the summaries according to inline depth
  for (auto &item : SI) {
    const GuardedValueFlowNode *Key = item.first;
    for (T *InSmry : item.second) {
      if (InSmry == nullptr) {
        continue;
      }
      unsigned int Depth = InSmry->getInlineDepth();
      if (Depth >= CARED_INLINE_DEPTH) {
        TmpSmry[CARED_INLINE_DEPTH][Key].insert(InSmry);
        Size[CARED_INLINE_DEPTH]++;
      } else {
        TmpSmry[Depth][Key].insert(InSmry);
        Size[Depth]++;
      }
    }
  }

  // Find the adaptive inline depth that ensures summary count less than
  // GSAFOptions::MaxSummary
  int Sum = 0;
  int Adaptive = 0;
  for (Adaptive = 0; Adaptive < CARED_INLINE_DEPTH + 1; Adaptive++) {
    Sum += Size[Adaptive];
    if (Sum > GSAFOptions::MaxSummary) {
      break;
    }
  }

  if (Adaptive < CARED_INLINE_DEPTH + 1) {
    SI.clear();
    for (int i = 0; i < Adaptive; i++) {
      if (Size[i] == 0) {
        // Empty set
        continue;
      }
      for (auto &item : TmpSmry[i]) {
        const GuardedValueFlowNode *Key = item.first;
        for (T *SmryItem : item.second) {
          SI[Key].insert(SmryItem);
        }
      }
    }
  }
}

void FunctionAnalyzer::finalizeSummary() {
  finalizeSummaryItem(Smry->InSmrys);
  finalizeSummaryItem(Smry->OutSmrys);
  finalizeSummaryItem(Smry->TaintSourceWrapperSmrys);
}

// General search function that can handle both forward and backward traversal.
// Node: Current node.
// PrevNode: Previous node in the traversal.
// Src: The source of the taint being tracked.
// InlineDepth: Current inline depth.
void FunctionAnalyzer::search(const GuardedValueFlowNode *Node,
                              const GuardedValueFlowNode *PrevNode,
                              Vulnerability::ValueSitePairType Src,
                              unsigned InlineDepth) {
  DEBUG_TRACE(dbgs() << "\t >Visiting[dw]: " << *Node << "...\n");
  if (TSV->checkNode(Node, TraceBuilder)) {
    DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *Node
                       << " by filtering nodes...\n");
    return;
  }

  if (PrevNode) {
    auto DepsCond = Solver->getDepsPair(PrevNode, Node);
    Solver->addAll(DepsCond.first);
    Solver->addAll(DepsCond.second);
  }
  inlineReturnSymbolicSummary(Src);
  TraceBuilder.add(Node);

  // search up
  pushState();
  TraceBuilder.push();
  Solver->push();

  bottomUpDepthFirstSearch(Node, PrevNode, Src, InlineDepth);

  Solver->pop();
  TraceBuilder.pop();
  popState();

  if (const GuardedValueFlowCallOutputNode *CSO =
          dyn_cast<GuardedValueFlowCallOutputNode>(Node)) {
    DEBUG_TRACE(dbgs() << "\t >It is a cso, inlining cso summaries...\n");

    // Using alias summary, continue to search down
    auto *GraphCS = callSite(CSO);
    for (auto CalleeIt = GraphCS->getCallees().begin(),
              CalleeEnd = GraphCS->getCallees().end();
         CalleeIt != CalleeEnd; ++CalleeIt) {
      Function *Callee = *CalleeIt;

      for (auto InputIt = GraphCS->input_begin(Callee),
                E = GraphCS->input_end(Callee);
           InputIt != E; ++InputIt) {
        auto *ActualNode = (*InputIt).InputNode;

        auto OutSmryIt =
            OutputSmryMap.find(std::make_tuple(ActualNode, CSO, Callee));
        if (OutSmryIt == OutputSmryMap.end()) {
          continue;
        }

        auto &OutSmryVec = OutSmryIt->second;
        for (auto *CalleeOutSmry : OutSmryVec) {
          inlineCalleeOutSummary(
              GraphCS, CalleeOutSmry, CSO, Src,
              std::max<unsigned>(CalleeOutSmry->getInlineDepth() + 1,
                                 InlineDepth),
              1);
        }
      }
    }
  } else {
    // for each of Node's child
    //      search child
    std::set<const GuardedValueFlowNode *> VisitedChildren;
    for (size_t I = 0; I < Node->getNumChildren(); ++I) {
      auto *Child = Node->getChild(I);

      if (VisitedChildren.count(Child)) {
        // children may repeat
        continue;
      }

      if (isa<GuardedValueFlowRegionNode>(Child)) {
        continue;
      }

      if (const GuardedValueFlowOpcodeNode *Opcode =
              dyn_cast<GuardedValueFlowOpcodeNode>(Child)) {
        if (!(Opcode->getOpcodeKind() ==
              GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr) &&
            !(Opcode->getKind() == GuardedValueFlowNode::Kind::CastOpcode) &&
            !(Opcode->getOpcodeKind() ==
              GuardedValueFlowOpcodeNode::OpcodeKind::Select)) {
          continue;
        }
      }

      if (const GuardedValueFlowOpcodeNode *Opcode =
              dyn_cast<GuardedValueFlowOpcodeNode>(Node)) {
        if ((Opcode->getOpcodeKind() ==
             GuardedValueFlowOpcodeNode::OpcodeKind::Select) &&
            Opcode->getOperand(0) == Child) {
          continue;
        }

        if ((Opcode->getOpcodeKind() ==
             GuardedValueFlowOpcodeNode::OpcodeKind::GetElementPtr) &&
            Opcode->getOperand(0) != Child) {
          continue;
        }
      }

      VisitedChildren.insert(Child);

      pushState();
      TraceBuilder.push();
      Solver->push();

      search(Child, Node, Src, InlineDepth);

      Solver->pop();
      TraceBuilder.pop();
      popState();
    }
  }
}

// Backward search to verify if a node can reach a source or sink under
// constraints.
void FunctionAnalyzer::bottomUpDepthFirstSearch(
    const GuardedValueFlowNode *Node, const GuardedValueFlowNode *PrevNode,
    Vulnerability::ValueSitePairType Src, unsigned InlineDepth) {
  TimeChecker->check();
  DEBUG_TRACE(dbgs() << "\t >Visiting[up]: " << *Node << "...\n");
  DEBUG_CONSTRAINTS(dbgs() << smtText(Solver->assertions()) << "\n");

  if (PrevNode && PrevNode->containsParent(Node)) {
    auto DepsCond = Solver->getDepsPair(Node, PrevNode);
    Solver->addAll(DepsCond.first);
    Solver->addAll(DepsCond.second);
  } else {
    auto DepsCond = Solver->getDepsPair(Node, nullptr);
    Solver->addAll(DepsCond.first);
    Solver->addAll(DepsCond.second);
  }
  inlineReturnSymbolicSummary(Src);

  if (TraceBuilder.recentObj() != Node) {
    TraceBuilder.add(Node);
  }

  std::unordered_map<const BasicBlock *, SMTSolver::SMTResultType>
      BlockResultMap;
  for (auto UIt = Node->useSites().begin(), UE = Node->useSites().end();
       UIt != UE; ++UIt) {
    auto *UseSite = *UIt;
    // special handling for return node: only the corresponding return site
    // is taken into consideration
    if (const GuardedValueFlowReturnNode *RN =
            dyn_cast<GuardedValueFlowReturnNode>(Node)) {
      assert(PrevNode && PrevNode->containsParent(Node));
      if (!std::any_of(
              RN->incomingReturns().begin(), RN->incomingReturns().end(),
              [&](const GuardedValueFlowReturnNode::ReturnIncoming &incoming) {
                return incoming.value == PrevNode && incoming.site == UseSite;
              })) {
        continue;
      }
    }

    DEBUG_TRACE(dbgs() << "\t\t >Detected User: " << *UseSite << "\n");

    // test use site type
    auto UTy = TSV->checkSite(UseSite, TraceBuilder);

    if (UTy == Vulnerability::ST_Others) {
      DEBUG_TRACE(dbgs() << "\t\t\t >Insensitive!\n");
      continue;
    }

    if (UTy == Vulnerability::ST_Call &&
        !hasSummary(dyn_cast<GuardedValueFlowCallSite>(UseSite), Node)) {
      DEBUG_TRACE(dbgs() << "\t\t\t >No summary! \n");
      continue;
    }

    Solver->push();
    pushState();

    // ensure that the block of the use site is reachable
    if (UseSite->getParentBasicBlock() != Node->getParentBasicBlock()) {
      auto Deps = ctrlDepsPair(*Solver, UseSite->getParentBasicBlock(), Graph,
                               Node->getParentBasicBlock());
      Solver->addAll(Deps.first);
      Solver->addAll(Deps.second);
    }
    // inline sym summary, all callee rets should be replaced (map ret/args)
    inlineReturnSymbolicSummary(Src);

    int PostUTyMask = checkUseSite((const GuardedValueFlowNode *)Node, UseSite,
                                   BlockResultMap, true);

    Solver->push();

    if (PostUTyMask && canReport(Src) && UTy == Vulnerability::ST_Sink) {
      TraceBuilder.push();
      TraceBuilder.add(UseSite);
      auto Trace = TraceBuilder.snapshot();
      TraceBuilder.pop();

      tryReport(Trace);

      Solver->pop();
    } else {

      Solver->pop();

      if (PostUTyMask) {
        processUseSite(Src, (const GuardedValueFlowNode *)Node, UseSite, UTy,
                       InlineDepth);
      } else {
        DEBUG_TRACE(dbgs() << "\t\t\t >Checking unsat!\n");
        if (Node->getParentBasicBlock() == UseSite->getParentBasicBlock()) {
          Solver->pop();
          popState();

          return;
        }
      }
    }

    Solver->pop();
    popState();
  }

  // continue to search
  auto FlowParents = valueFlowParents(Node, GSAFOptions::EnableArithmeticFlow);
  for (auto It = FlowParents.begin(), Next = It, E = FlowParents.end(); It != E;
       It = Next) {
    ++Next;
    const GuardedValueFlowNode *ParentNode = *It;
    if (ParentNode == PrevNode) {
      // this is special for bi-direction search
      // in case of repeating searching the same path.
      continue;
    }

    auto ResIt = BlockResultMap.find(ParentNode->getParentBasicBlock());
    if (ResIt != BlockResultMap.end() &&
        ResIt->second != SMTSolver::SMTRT_Sat) {
      DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *ParentNode
                         << " by cached results...\n");
      continue;
    }

    if (TSV->checkNode(ParentNode, TraceBuilder)) {
      DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *ParentNode
                         << " by filtering nodes...\n");
      continue;
    }

    if (Next != E) {
      TraceBuilder.push();
      Solver->push();
      pushState();
    }

    bottomUpDepthFirstSearch(ParentNode, Node, Src, InlineDepth);

    if (Next != E) {
      Solver->pop();
      TraceBuilder.pop();
      popState();
    }
  }
  return;
}

int FunctionAnalyzer::checkUseSite(
    const GuardedValueFlowNode *Node, const GuardedValueFlowSite *UseSite,
    std::unordered_map<const BasicBlock *, SMTSolver::SMTResultType>
        &BlockResultMap,
    bool UseCache) {
  BasicBlock *UseSiteBlock = UseSite->getParentBasicBlock();

  auto Result = SMTSolver::SMTRT_Uncheck;

  if (UseCache) {
    auto ResultIt = BlockResultMap.find(UseSiteBlock);
    if (ResultIt != BlockResultMap.end()) {
      Result = ResultIt->second;
    }
  }

  int SiteCanReach = 1;

  switch (Result) {
  case SMTSolver::SMTRT_Uncheck: {
    Solver->push();

    SMTExprVec Prereq = Solver->getSMTFactory().createEmptySMTExprVec();
    TSV->setPrerequisites(Solver, UseSite, TraceBuilder, Prereq);
    Solver->addAll(Prereq);
    auto Result = Solver->check();

    BlockResultMap[UseSiteBlock] =
        (Result == SMTSolver::SMTRT_Sat ? SMTSolver::SMTRT_Sat
                                        : SMTSolver::SMTRT_Unsat);

    if (Result != SMTSolver::SMTRT_Sat) {
      SiteCanReach = 0;
    }

    if (Node->getParentBasicBlock() == UseSiteBlock &&
        !properlyDominatedBlocks(PDT, UseSiteBlock).empty()) {
      // Caching Result to all BasicBlocks that post-dom InstBlock,
      auto DominanceRange5 = properlyDominatedBlocks(PDT, UseSiteBlock);
      for (auto I = DominanceRange5.begin(), E = DominanceRange5.end(); I != E;
           I++) {
        BlockResultMap[*I] =
            (Result == SMTSolver::SMTRT_Sat ? SMTSolver::SMTRT_Sat
                                            : SMTSolver::SMTRT_Unsat);
      }
    }

    if (Result != SMTSolver::SMTRT_Sat &&
        !properDominators(DT, UseSiteBlock).empty()) {
      auto DominanceRange6 = properDominators(DT, UseSiteBlock);
      for (auto I = DominanceRange6.begin(), E = DominanceRange6.end(); I != E;
           I++) {
        BlockResultMap[*I] = (SMTSolver::SMTRT_Unsat);
      }
    }
    Solver->pop();
  } break;
  case SMTSolver::SMTRT_Sat:
    break;
  case SMTSolver::SMTRT_Unsat:
  case SMTSolver::SMTRT_Unknown:
    SiteCanReach = 0;
    break;
  }
  return SiteCanReach;
}

void FunctionAnalyzer::processUseSite(Vulnerability::ValueSitePairType Src,
                                      const GuardedValueFlowNode *Node,
                                      const GuardedValueFlowSite *UseSite,
                                      Vulnerability::SiteType USTy,
                                      unsigned InlineDepth) {
  auto *SrcNode = Src.first;
  auto *SrcSite = Src.second;

  if ((USTy & Vulnerability::ST_Return)) {
    DEBUG_TRACE(dbgs() << "\t\t *Detected Ret-Inst: " << *UseSite << "\n");
    DEBUG_CONSTRAINTS(dbgs() << OUT_SUMMARY_SEP_START << "\n");
    DEBUG_CONSTRAINTS(dbgs() << smtText(Solver->assertions()) << "\n");
    DEBUG_CONSTRAINTS(dbgs() << OUT_SUMMARY_SEP_END << "\n");

    TraceBuilder.push();
    TraceBuilder.add(UseSite);

    OutputSummary *OutSmry =
        createOutputSummary(Solver->assertions(), TraceBuilder.snapshot(),
                            &Solver->getUsedFunctionArguments(), InlineDepth);

    addCachedConstraintsToSummary(OutSmry);

    DEBUG_TRACE(dbgs() << *OutSmry << "\n");

    Smry->OutSmrys[Node].insert(OutSmry);

    TraceBuilder.pop();
  }

  if ((USTy & Vulnerability::ST_Sink)) {
    DEBUG_TRACE(dbgs() << "\t\t *Detected Sink-Inst: " << *UseSite << "\n");

    TraceBuilder.push();
    TraceBuilder.add(UseSite);

    InputSummary *InSmry =
        createInputSummary(Solver->assertions(), TraceBuilder.snapshot(),
                           &Solver->getUsedFunctionArguments(), InlineDepth);

    addCachedConstraintsToSummary(InSmry);

    DEBUG_TRACE(dbgs() << *InSmry << "\n");
    //		report(InSmry, UseSite, Src, false);

    Smry->InSmrys[(isa<GuardedValueFlowArgumentNode>(SrcNode) && !SrcSite)
                      ? SrcNode
                      : nullptr]
        .insert(InSmry);

    TraceBuilder.pop();
  }

  if ((USTy & Vulnerability::ST_Call)) {
    DEBUG_TRACE(dbgs() << "\t\t *Detected Call-Inst: " << *UseSite << "\n");

    auto *GraphCS = dyn_cast<GuardedValueFlowCallSite>(UseSite);
    if (!GraphCS) {
      errs() << *UseSite << "\n";
      llvm_unreachable("ST_Call is not a call site");
    }

    // input summary of the instruction from Node
    if (!std::static_pointer_cast<TaintExtras>(TSV)->isSinkFunction(
            Graph, dyn_cast<GuardedValueFlowArgumentNode>(SrcNode))) {
      if (!SrcSite || Parent->isReachable(SrcSite->getInstruction(),
                                          UseSite->getInstruction())) {

        auto InSmryIt = InputSmryMap.find(std::make_pair(Node, GraphCS));
        if (InSmryIt != InputSmryMap.end()) {
          auto &InSmryVec = InSmryIt->second;
          for (auto *CalleeInSmry : InSmryVec) {
            inlineCalleeInSummary(
                GraphCS, CalleeInSmry, Src, (const GuardedValueFlowNode *)Node,
                std::max<unsigned>(CalleeInSmry->getInlineDepth() + 1,
                                   InlineDepth));
          }
        }
        DEBUG_TRACE(
            dbgs() << "\t\t ---------------------------------------------\n");
      }
    }

    // if Node + UseSite -> taint site : produce a new kind of output summary
    if (isa<GuardedValueFlowArgumentNode>(SrcNode) && !SrcSite) {
      auto SourceWrapperIt = SourceWrapperSmryMap.find(GraphCS);
      if (SourceWrapperIt != SourceWrapperSmryMap.end()) {
        auto SmryIt = SourceWrapperIt->second.find(Node);
        if (SmryIt != SourceWrapperIt->second.end()) {
          auto &InSmryVec = SmryIt->second;
          for (auto *CalleeInSmry : InSmryVec) {
            DEBUG_TRACE(dbgs() << "\t\t " << TAINT_WRAPPER_STR << "[A]\n");
            inlineCalleeInSummary(
                GraphCS, CalleeInSmry, Src, (const GuardedValueFlowNode *)Node,
                std::max<unsigned>(CalleeInSmry->getInlineDepth() + 1,
                                   InlineDepth),
                2);
          }
        }
      }

      auto SrcIt = Sources.find(GraphCS);
      if (SrcIt != Sources.end() && SrcIt->second.count(Node)) {
        TraceBuilder.push();
        TraceBuilder.add(UseSite);

        DEBUG_TRACE(dbgs() << "\t\t " << TAINT_WRAPPER_STR << "[B]\n");

        InputSummary *InSmry = createInputSummary(
            Solver->assertions(), TraceBuilder.snapshot(),
            &Solver->getUsedFunctionArguments(), InlineDepth);

        addCachedConstraintsToSummary(InSmry);

        DEBUG_TRACE(dbgs() << *InSmry << "\n");

        Smry->TaintSourceWrapperSmrys[SrcNode].insert(InSmry);

        TraceBuilder.pop();
      }
    }

    // output summary of the instruction from Node
    for (auto CalleeIt = GraphCS->getCallees().begin(),
              CalleeEnd = GraphCS->getCallees().end();
         CalleeIt != CalleeEnd; ++CalleeIt) {
      Function *Callee = *CalleeIt;
      auto NativeRange1 = callOutputs(GraphCS, Callee);
      for (auto OutputIt = NativeRange1.begin(), E = NativeRange1.end();
           OutputIt != E; ++OutputIt) {
        auto *OutputNode = *OutputIt;

        auto OutSmryIt =
            OutputSmryMap.find(std::make_tuple(Node, OutputNode, Callee));
        if (OutSmryIt == OutputSmryMap.end()) {
          continue;
        }

        auto &OutSmryVec = OutSmryIt->second;
        for (auto *CalleeOutSmry : OutSmryVec) {
          inlineCalleeOutSummary(
              GraphCS, CalleeOutSmry, OutputNode, Src,
              std::max<unsigned>(CalleeOutSmry->getInlineDepth() + 1,
                                 InlineDepth));
        }
      }
    }

    DEBUG_TRACE(
        dbgs() << "\t\t *********************************************\n");
  }

  return;
}

bool FunctionAnalyzer::canReport(Vulnerability::ValueSitePairType Src) const {
  return !(isa<GuardedValueFlowArgumentNode>(Src.first) && !Src.second);
}

void FunctionAnalyzer::report(InputSummary *Smry,
                              const GuardedValueFlowSite *UseSite,
                              Vulnerability::ValueSitePairType Src, bool Race) {
  if (canReport(Src)) {
    if (Race) {
      // If the trace comes from an arg-symbol summary,
      // we should check if the trace is reported.
      // If reported, break, else, mark as reported.
      // Do not forget synchronization.
      std::shared_ptr<VulnerabilityTrace> Trace = Smry->getTrace();
      Trace->lock();
      if (!Trace->reported()) {
        Trace->setReported(true);
        BugTraces.push_back(std::make_pair(1, Trace));
        DEBUG_TRACE(dbgs() << "\t\t " << BUG_STR << *UseSite << "\n");
      }
      Trace->unlock();
    } else {
      std::shared_ptr<VulnerabilityTrace> Trace = Smry->getTrace();
      Trace->setReported(true);
      BugTraces.push_back(std::make_pair(1, Trace));

      DEBUG_TRACE(dbgs() << "\t\t " << BUG_STR << *UseSite << "\n");
    }
  }
  return;
}

void FunctionAnalyzer::initSummary() {
  auto NativeRange2 = callSites(Graph);
  for (auto CSIt = NativeRange2.begin(), CSE = NativeRange2.end(); CSIt != CSE;
       ++CSIt) {
    auto *GraphCS = *CSIt;
    for (auto CalleeIt = GraphCS->getCallees().begin(),
              CalleeEnd = GraphCS->getCallees().end();
         CalleeIt != CalleeEnd; ++CalleeIt) {
      Function *Callee = *CalleeIt;
      auto NativeRange3 = callOutputs(GraphCS, Callee);
      for (auto It = NativeRange3.begin(), E = NativeRange3.end(); It != E;
           ++It) {
        initOutputSummary(GraphCS, Callee, *It);
      }

      for (auto It = GraphCS->input_begin(Callee),
                E = GraphCS->input_end(Callee);
           It != E; ++It) {
        auto &InputStruct = *It;
        initInputSummary(GraphCS, Callee, InputStruct.InputNode,
                         InputStruct.InputIndex, InputStruct.IsCommonInput);
        initSourceWrapperSummary(GraphCS, Callee, InputStruct.InputNode,
                                 InputStruct.InputIndex,
                                 InputStruct.IsCommonInput);
      }
    }
  }
}

void FunctionAnalyzer::initOutputSummary(
    const GuardedValueFlowCallSite *GraphCS, Function *Callee,
    const GuardedValueFlowCallOutputNode *Node) {
  assert(Node && Callee);
  assert((!GraphCS->getCalledFunction() ||
          GraphCS->getCalledFunction() == Callee) &&
         "Direct call has only one callee.");

  if (GraphCS->isBackEdge(Callee)) {
    return;
  }
  //  auto *CallerNode = Parent->CG->getOrInsertFunction(F);
  //  auto *CalleeNode = Parent->CG->getOrInsertFunction(Callee);
  //  if (CallerNode->containsCallProperty(GraphCS->getLLVMCallSite(),
  //  CalleeNode,
  //                                       CBCallGraphNode::CPT_BackEdge)) {
  //    // Do not consider back-edge.
  //    //
  //    // Consider call graph: B <--> A --> C --> B,
  //    // If both B --> A and A --> B are marked as
  //    // back-edge, A cannot obtain the summary
  //    // of B, because the summary of B may have been
  //    // released after C is analyzed.
  //    return;
  //  }

  auto It = Parent->FuncSmryMap.find(Callee);
  if (It == Parent->FuncSmryMap.end()) {
    // This may happen in following two cases
    // (1) recursive call (2) empty function
    return;
  }

  GSAFSummary *AllSmry = It->second;
  assert(AllSmry);

  auto *CalleeGraph = Parent->getGraph(Callee);
  auto *RetNode =
      nodeOfKind<GuardedValueFlowNode::Kind::CallSiteCommonOutput,
                 GuardedValueFlowCallOutputNode>(Node)
          ? (const GuardedValueFlowNode *)CalleeGraph->getCommonReturn()
          : (const GuardedValueFlowNode *)CalleeGraph->getPseudoReturn(
                Node->getIndex());
  assert(RetNode);
  auto SmryIt = AllSmry->OutSmrys.find(RetNode);
  if (SmryIt == AllSmry->OutSmrys.end()) {
    return;
  }

  for (auto *OutSmry : SmryIt->second) {
    auto *SrcNode = OutSmry->getSourceNode();
    if (!isa<GuardedValueFlowArgumentNode>(SrcNode)) {
      if (OutSmry->getInlineDepth() >= GSAFOptions::InlineDepth) {
        continue;
      }
      SourceOutSmryVec.emplace_back(Node, OutSmry);
    } else {
      assert(isa<GuardedValueFlowArgumentNode>(SrcNode));
      if (nodeOfKind<GuardedValueFlowNode::Kind::VariableArgument,
                     GuardedValueFlowNode>(SrcNode)) {
        for (size_t I = 0; I < GraphCS->getCommonInputs().size(); I++) {
          OutputSmryMap[std::make_tuple(GraphCS->getCommonInputs()[I], Node,
                                        Callee)]
              .insert(OutSmry);
        }
      } else if (nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                            GuardedValueFlowNode>(SrcNode)) {
        OutputSmryMap[std::make_tuple(
                          GraphCS->getCommonInput(
                              ((const GuardedValueFlowNode *)SrcNode)
                                  ->getIndex()),
                          Node, Callee)]
            .insert(OutSmry);
      } else {
        assert((nodeOfKind<GuardedValueFlowNode::Kind::PseudoArgument,
                           GuardedValueFlowNode>(SrcNode)));
        size_t Index = ((const GuardedValueFlowNode *)SrcNode)->getIndex();

        if (auto *PseudoInput = GraphCS->getPseudoInput(Callee, Index)) {
          OutputSmryMap[std::make_tuple(PseudoInput, Node, Callee)].insert(
              OutSmry);
        }
      }
    }
  }
}

void FunctionAnalyzer::initInputSummary(const GuardedValueFlowCallSite *GraphCS,
                                        Function *Callee,
                                        const GuardedValueFlowNode *ActualArg,
                                        std::size_t ArgIndex,
                                        bool CommonActual) {
  assert(ActualArg && Callee);

  if (GraphCS->isBackEdge(Callee)) {
    return;
  }

  //  auto *CallerNode = Parent->CG->getOrInsertFunction(F);
  //  auto *CalleeNode = Parent->CG->getOrInsertFunction(Callee);
  //  if (CallerNode->containsCallProperty(GraphCS->getLLVMCallSite(),
  //  CalleeNode,
  //                                       CBCallGraphNode::CPT_BackEdge)) {
  //    // Do not consider back-edge.
  //    //
  //    // Consider call graph: B <--> A --> C --> B,
  //    // If both B --> A and A --> B are marked as
  //    // back-edge, A cannot obtain the summary
  //    // of B, because the summary of B may have been
  //    // released after C is analyzed.
  //    return;
  //  }

  auto It = Parent->FuncSmryMap.find(Callee);
  if (It == Parent->FuncSmryMap.end()) {
    // This may happen in following two cases
    // (1) recursive call (2) empty function
    return;
  }

  GSAFSummary *AllSmry = It->second;
  assert(AllSmry);

  auto *CalleeGraph = Parent->getGraph(Callee);
  size_t NumArgument = CommonActual ? CalleeGraph->getNumCommonArgument()
                                    : CalleeGraph->getNumPseudoArgument();
  if (ArgIndex < NumArgument) {
    const GuardedValueFlowNode *Formal =
        CommonActual
            ? (const GuardedValueFlowNode *)CalleeGraph->getCommonArgument(
                  ArgIndex)
            : (const GuardedValueFlowNode *)CalleeGraph->getPseudoArgument(
                  ArgIndex);

    auto SmryIt = AllSmry->InSmrys.find(Formal);
    if (SmryIt != AllSmry->InSmrys.end()) {
      for (auto *CalleeInSmry : SmryIt->second) {
        InputSmryMap[std::make_pair(ActualArg, GraphCS)].insert(CalleeInSmry);
      }
    }
  } else {
    assert(CommonActual);
    for (size_t AI = 0, AE = CalleeGraph->getNumVarArgument(); AI != AE; AI++) {
      auto *Formal = CalleeGraph->getVarArgument(AI);
      assert((nodeOfKind<GuardedValueFlowNode::Kind::VariableArgument,
                         GuardedValueFlowNode>(Formal)));
      auto SmryIt = AllSmry->InSmrys.find(Formal);
      if (SmryIt != AllSmry->InSmrys.end()) {
        for (auto *CalleeInSmry : SmryIt->second) {
          InputSmryMap[std::make_pair(ActualArg, GraphCS)].insert(CalleeInSmry);
        }
      }
    }
  }
}

void FunctionAnalyzer::initSourceWrapperSummary(
    const GuardedValueFlowCallSite *GraphCS, Function *Callee,
    const GuardedValueFlowNode *ActualArg, std::size_t ArgIndex,
    bool CommonActual) {
  assert(ActualArg && Callee);

  if (GraphCS->isBackEdge(Callee)) {
    return;
  }

  //  auto *CallerNode = Parent->CG->getOrInsertFunction(F);
  //  auto *CalleeNode = Parent->CG->getOrInsertFunction(Callee);
  //  if (CallerNode->containsCallProperty(GraphCS->getLLVMCallSite(),
  //  CalleeNode,
  //                                       CBCallGraphNode::CPT_BackEdge)) {
  //    // Do not consider back-edge.
  //    //
  //    // Consider call graph: B <--> A --> C --> B,
  //    // If both B --> A and A --> B are marked as
  //    // back-edge, A cannot obtain the summary
  //    // of B, because the summary of B may have been
  //    // released after C is analyzed.
  //    return;
  //  }

  auto It = Parent->FuncSmryMap.find(Callee);
  if (It == Parent->FuncSmryMap.end()) {
    // This may happen in following two cases
    // (1) recursive call (2) empty function
    return;
  }

  GSAFSummary *AllSmry = It->second;
  assert(AllSmry);

  auto *CalleeGraph = Parent->getGraph(Callee);
  size_t NumArgument = CommonActual ? CalleeGraph->getNumCommonArgument()
                                    : CalleeGraph->getNumPseudoArgument();
  if (ArgIndex < NumArgument) {
    const GuardedValueFlowNode *Formal =
        CommonActual
            ? (const GuardedValueFlowNode *)CalleeGraph->getCommonArgument(
                  ArgIndex)
            : (const GuardedValueFlowNode *)CalleeGraph->getPseudoArgument(
                  ArgIndex);

    auto SmryIt = AllSmry->TaintSourceWrapperSmrys.find(Formal);
    if (SmryIt != AllSmry->TaintSourceWrapperSmrys.end()) {
      for (auto *CalleeInSmry : SmryIt->second) {
        SourceWrapperSmryMap[GraphCS][ActualArg].insert(CalleeInSmry);
      }
    }
  } else {
    assert(CommonActual);
    for (size_t AI = 0, AE = CalleeGraph->getNumVarArgument(); AI != AE; AI++) {
      auto *Formal = CalleeGraph->getVarArgument(AI);
      assert((nodeOfKind<GuardedValueFlowNode::Kind::VariableArgument,
                         GuardedValueFlowNode>(Formal)));
      auto SmryIt = AllSmry->TaintSourceWrapperSmrys.find(Formal);
      if (SmryIt != AllSmry->TaintSourceWrapperSmrys.end()) {
        for (auto *CalleeInSmry : SmryIt->second) {
          SourceWrapperSmryMap[GraphCS][ActualArg].insert(CalleeInSmry);
        }
      }
    }
  }
}

void FunctionAnalyzer::matchFormalActual(
    SummaryBase *Smry, const GuardedValueFlowCallSite *GraphCS,
    const std::string &RenameSuffix) {
  // mapping actual to formal
  auto *CalleeGraph = Parent->getGraph(Smry->getFunction());
  for (auto *Formal : Smry->getInputs()) {
    const std::string &FormalDesc = detail::encodingSymbol(Formal);
    SMTExpr FormalExpr = Solver->getSMTFactory().createBitVecConst(
        FormalDesc + RenameSuffix,
        Parent->DL->getTypeSizeInBits(Formal->getType()));
    if (nodeOfKind<GuardedValueFlowNode::Kind::VariableArgument,
                   GuardedValueFlowNode>(Formal)) {
      unsigned VarActualArgStart = CalleeGraph->getNumCommonArgument();
      unsigned VarActualArgEnd = GraphCS->getCommonInputs().size();
      if (VarActualArgStart < VarActualArgEnd) {
        unsigned Idx = VarActualArgStart;
        SMTExpr Mapping = (FormalExpr == Solver->getOrInsertExpr(
                                             GraphCS->getCommonInputs()[Idx]));
        for (; Idx < VarActualArgEnd; Idx++) {
          auto *ActualNode = GraphCS->getCommonInputs()[Idx];
          assert(ActualNode);
          Mapping =
              Mapping || (FormalExpr == Solver->getOrInsertExpr(ActualNode));

          Solver->addAll(Solver->getDataDeps(ActualNode));
        }
        Solver->add(Mapping);
      }
    } else {
      const GuardedValueFlowNode *ActualNode = nullptr;
      if (nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                     GuardedValueFlowNode>(Formal)) {
        ActualNode = GraphCS->getCommonInput(Formal->getIndex());
      } else {
        ActualNode =
            GraphCS->getPseudoInput(Smry->getFunction(), Formal->getIndex());
      }
      if (!ActualNode) {
        continue;
      }

      SMTExpr Mapping = (FormalExpr == Solver->getOrInsertExpr(ActualNode));
      Solver->add(Mapping);

      Solver->addAll(Solver->getDataDeps(ActualNode));
    }
  }
}

// Helper to check if a call site has relevant summaries to inline.
bool FunctionAnalyzer::hasSummary(const GuardedValueFlowCallSite *GraphCS,
                                  const GuardedValueFlowNode *Node) {
  assert(GraphCS);
  Function *Callee = GraphCS->getCalledFunction();
  if (Callee == F) {
    return false;
  }

  auto SrcIt = Sources.find(GraphCS);
  if (SrcIt != Sources.end() &&
      SrcIt->second.count(dyn_cast<GuardedValueFlowNode>(Node))) {
    return true;
  }

  auto InSmryIt = InputSmryMap.find(std::make_pair(Node, GraphCS));
  if (InSmryIt != InputSmryMap.end() && !InSmryIt->second.empty()) {
    return true;
  }

  auto SourceWrapperIt = SourceWrapperSmryMap.find(GraphCS);
  if (SourceWrapperIt != SourceWrapperSmryMap.end()) {
    auto SmrySetIt = SourceWrapperIt->second.find(Node);
    if (SmrySetIt != SourceWrapperIt->second.end()) {
      return !SmrySetIt->second.empty();
    }
  }

  for (auto CalleeIt = GraphCS->getCallees().begin(),
            CalleeEnd = GraphCS->getCallees().end();
       CalleeIt != CalleeEnd; ++CalleeIt) {
    Function *Callee = *CalleeIt;
    auto NativeRange4 = callOutputs(GraphCS, Callee);
    for (auto It = NativeRange4.begin(), E = NativeRange4.end(); It != E;
         ++It) {
      auto OutSmryIt = OutputSmryMap.find(std::make_tuple(Node, *It, Callee));

      if (OutSmryIt != OutputSmryMap.end() && !OutSmryIt->second.empty()) {
        return true;
      }
    }
  }

  return false;
}

bool FunctionAnalyzer::inlineReturnSymbolicSummary(
    const GuardedValueFlowCallOutputNode *Node) {
  if (!GSAFOptions::EnableCSSymSummary) {
    return false;
  }

  assert(Node);
  assert(Node->getGraph() == Graph);

  if (CallSiteOutputCache.contains(Node)) {
    return false;
  }

  const GuardedValueFlowCallSite *CS = callSite(Node);
  assert(CS);
  int CSIdx = -1;
  if (nodeOfKind<GuardedValueFlowNode::Kind::CallSitePseudoOutput,
                 GuardedValueFlowCallOutputNode>(Node)) {
    CSIdx = Node->getIndex();
  }

  Function *Callee = CS->getCalledFunction();
  if (!Callee || Callee == F || Callee->empty()) {
    // only consider direct call
    return false;
  }

  if (CS->isBackEdge(Callee)) {
    return false;
  }

  //  if (Parent->CG->getOrInsertFunction(F)->containsCallProperty(
  //          CS->getLLVMCallSite(), Parent->CG->getOrInsertFunction(Callee),
  //          CBCallGraphNode::CPT_BackEdge)) {
  //    // Do not consider back-edge.
  //    //
  //    // Consider call graph: B <--> A --> C --> B,
  //    // If both B --> A and A --> B are marked as
  //    // back-edge, A cannot obtain the summary
  //    // of B, because the summary of B may have been
  //    // released after C is analyzed.
  //    return false;
  //  }

  const GuardedValueFlowReturnNode *RetNodeInCallee = nullptr;
  if (CSIdx >= 0) {
    RetNodeInCallee = Parent->getGraph(Callee)->getPseudoReturn(CSIdx);
  } else {
    RetNodeInCallee = Parent->getGraph(Callee)->getCommonReturn();
  }

  auto It = Parent->FuncSmryMap.find(Callee);
  assert(It != Parent->FuncSmryMap.end());
  GSAFSummary *AllSmry = It->second;
  assert(AllSmry);
  auto SymSmryPair = AllSmry->RetSmrys.find(RetNodeInCallee);
  if (SymSmryPair == AllSmry->RetSmrys.end()) {
    // This may happen in SCC.
    return false;
  }

  std::string RenamingSuffix = format_str("_CS%p", CS->getInstruction());

  for (const SummaryCacheItem &item :
       SymSmryPair->second->getNonSymDepsCache()) {
    SymbDepsCache.push_back(SummaryCacheItem(
        item.constraints, item.suffix + RenamingSuffix, item.depth + 1));
  }
  for (const SummaryCacheItem &item : SymSmryPair->second->getSymbDepsCache()) {
    SymbDepsCache.push_back(SummaryCacheItem(
        item.constraints, item.suffix + RenamingSuffix, item.depth + 1));
  }

  matchFormalActual(SymSmryPair->second, CS, RenamingSuffix);

  SMTExpr FormalRetExpr = Solver->getSMTFactory().createBitVecConst(
      detail::encodingSymbol(SymSmryPair->first) + RenamingSuffix,
      Parent->DL->getTypeSizeInBits(SymSmryPair->first->getType()));
  SMTExpr NodeExpr = Solver->getOrInsertExpr(Node);

  assert(FormalRetExpr.isBitVector() && NodeExpr.isBitVector());
  assert(FormalRetExpr.getBitVecSize() == NodeExpr.getBitVecSize());

  Solver->add(FormalRetExpr == NodeExpr);
  return true;
}

bool FunctionAnalyzer::inlineReturnSymbolicSummary(
    Vulnerability::ValueSitePairType Src) {
  if (!GSAFOptions::EnableCSSymSummary) {
    return false;
  }

  const GuardedValueFlowCallOutputNode *CurrentSrcSiteOutput = nullptr;
  if (Src.second) {
    if (auto *SrcSite = dyn_cast<GuardedValueFlowCallSite>(Src.second)) {
      CurrentSrcSiteOutput = dyn_cast_or_null<GuardedValueFlowCallOutputNode>(
          SrcSite->getCommonOutput());
    }
  }

  bool Inlined = false;
  unsigned NumToInline = GSAFOptions::InlineCSSymDepth;
  unsigned NumInlined = 0;

  if (NumInlined == NumToInline) {
    return false;
  }

  // Iterators in Range may become invalid, because the container
  // may become larger/smaller during inline.
  std::list<const GuardedValueFlowCallOutputNode *> CommonCSOCache;
  std::list<const GuardedValueFlowCallOutputNode *> PseudoCSOCache;

  typedef std::pair<
      std::vector<const GuardedValueFlowCallOutputNode *>::iterator,
      std::vector<const GuardedValueFlowCallOutputNode *>::iterator>
      RangeTy;
  auto doInitialization = [&CommonCSOCache, &PseudoCSOCache,
                           CurrentSrcSiteOutput, this](RangeTy Range) {
    CommonCSOCache.clear();
    PseudoCSOCache.clear();

    for (auto &It = Range.first; It != Range.second; ++It) {
      auto *CSO = *It;
      if (Function *CSOFunc = callSite(CSO)->getCalledFunction()) {
        // No summary for an output of a library function call
        if (CSOFunc->empty()) {
          continue;
        }
      }

      if (nodeOfKind<GuardedValueFlowNode::Kind::CallSiteCommonOutput,
                     GuardedValueFlowCallOutputNode>(CSO)) {
        if (Sources.count(callSite(CSO)) ||
            SourceWrapperSmryMap.count(callSite(CSO))) {
          if (CommonCSOCache.front() == CurrentSrcSiteOutput) {
            auto It = CommonCSOCache.begin();
            ++It;
            CommonCSOCache.insert(It, CSO);
          } else {
            CommonCSOCache.push_front(CSO);
          }
        } else {
          CommonCSOCache.push_back(CSO);
        }
      } else {
        PseudoCSOCache.push_back(CSO);
      }
    }
  };

  auto doInline =
      [this, &Inlined, &NumToInline, &NumInlined](
          std::list<const GuardedValueFlowCallOutputNode *> &CSOCache) {
        for (auto *CSO : CSOCache) {
          if (inlineReturnSymbolicSummary(CSO)) {
            NumInlined++;

            if (!Inlined)
              Inlined = true;

            if (NumInlined == NumToInline) {
              break;
            }
          }
        }
      };

  doInitialization(Solver->getUsedCallSiteOutputs(false));

  while (!CommonCSOCache.empty() || !PseudoCSOCache.empty()) {
    doInline(CommonCSOCache);

    if (NumInlined == NumToInline) {
      break;
    }

    doInline(PseudoCSOCache);

    if (NumInlined == NumToInline) {
      break;
    }

    // reinit
    doInitialization(Solver->getUsedCallSiteOutputs(false));
  }

  if (NumInlined == NumToInline) {
    // The limit is reached. Get one more time to notify the remaining ones
    // will not need to be inlined.
    Solver->getUsedCallSiteOutputs(false);
  }
  return Inlined;
}

void FunctionAnalyzer::buildReturnSymbolicSummary() {
  if (!GSAFOptions::EnableCSSymSummary) {
    return;
  }

  Solver->reset();
  resetState();

  for (auto It = Graph->return_begin(), E = Graph->return_end(); It != E;
       ++It) {
    TimeChecker->check();

    const GuardedValueFlowReturnNode *Ret = *It;
    assert(Ret);

    pushState();
    Solver->push();
    Solver->addAll(Solver->getDataDeps(Ret));

    inlineReturnSymbolicSummary({nullptr, nullptr});

    SymbolicSummary *SymSmry = new SymbolicSummary(F, GSAFOptions::InlineDepth);

    addCachedConstraintsToSummary(SymSmry);

    Smry->RetSmrys.insert(std::make_pair(Ret, SymSmry));
    auto NonSymDep = Solver->assertions().toAndExpr();
    SymSmry->addNonSymDeps(SummaryCacheItem(&NonSymDep, "", 0));
    SymSmry->setInputs(Solver->getUsedFunctionArguments());
    Solver->pop();
    popState();
  }
}

// Try to report a vulnerability if a complete trace is found.
// It uses SMT solver to verify the feasibility of the trace.
void FunctionAnalyzer::tryReport(std::shared_ptr<VulnerabilityTrace> Trace) {
  TimeChecker->check();
  if (SMTTimeChecker->isTimeOut())
    return;

  SMTFactory *F = new SMTFactory;
  SMTSolver *S = new SMTSolver(F->createSMTSolver());

  SMTAdvisor4Inlining *SMTAdviser = &Parent->SMTAdvisor;

  SMTExprVec *NonSymDep = new SMTExprVec(F->createEmptySMTExprVec());
  SMTExprVec *SymbDep = new SMTExprVec(F->createEmptySMTExprVec());

  for (SummaryCacheItem item : NonSymDepsCache.getCacheVector()) {
    SummaryBase::SMTReadLock();
    SMTExpr Translate = F->translate(*item.constraints);
    SummaryBase::SMTReadUnlock();

    SMTExprVec TempVec = F->createEmptySMTExprVec();
    TempVec.push_back(Translate);
    std::unordered_map<std::string, SMTExpr> Mapping;
    NonSymDep->push_back(
        F->rename(TempVec, item.suffix, Mapping, SMTAdviser).first.toAndExpr());
  }

  for (SummaryCacheItem item : SymbDepsCache.getCacheVector()) {
    SummaryBase::SMTReadLock();
    SMTExpr Translate = F->translate(*item.constraints);
    SummaryBase::SMTReadUnlock();

    SMTExprVec TempVec = F->createEmptySMTExprVec();
    TempVec.push_back(Translate);
    std::unordered_map<std::string, SMTExpr> Mapping;
    SymbDep->push_back(
        F->rename(TempVec, item.suffix, Mapping, SMTAdviser).first.toAndExpr());
  }

  S->addAll(F->translate(Solver->assertions()));

  TraceList<std::pair<int, std::shared_ptr<VulnerabilityTrace>>> *Reports =
      &BugTraces;

  TimeChecker->suspend();
  SMTTimeChecker->resume();
  Parent->ReportTasks.async([Trace, Reports, NonSymDep, SymbDep, F, S] {
    bool ShouldReport = true;

    if (!NonSymDep->empty()) {
      S->addAll(*NonSymDep);

      auto Result = S->check();

      if (Result != SMTSolver::SMTRT_Sat) {
        ShouldReport = false;
      }
    }

    if (ShouldReport) {
      if (!SymbDep->empty()) {
        S->addAll(*SymbDep);
        auto Result = S->check();

        if (Result != SMTSolver::SMTRT_Sat) {
          ShouldReport = false;
        }
      }

      if (ShouldReport) {
        // report
        SummaryBase::SMTWriteLock();
        Trace->setReported(true);
        Reports->push_back(std::make_pair(1, Trace));
        SummaryBase::SMTWriteUnlock();
      }
    }

    delete NonSymDep;
    delete SymbDep;
    delete S;
    delete F;
  });
  SMTTimeChecker->suspend();
  TimeChecker->resume();
}

// Inlines input summaries.
// Case 0: Normal input summary (backward propagation).
// Case 1: Continue forward search after inlining (used when searching from
// output to input). Case 2: Taint Source Wrapper summary (special handling).
void FunctionAnalyzer::inlineCalleeInSummary(
    const GuardedValueFlowCallSite *CS, InputSummary *CalleeInSmry,
    Vulnerability::ValueSitePairType Src,
    const GuardedValueFlowNode *ActualNode, unsigned Depth, int Case) {
  TimeChecker->check();

  if (Case != 1 && CalleeInSmry->getInlineDepth() >= GSAFOptions::InlineDepth) {
    return;
  }

  // translating, pruning, and mapping
  // copy to the current context and renaming for context sensitivity
  std::string RenamingSuffix = format_str("_CS%p", CS->getInstruction());
  std::unordered_map<std::string, SMTExpr> VariableMapping;

  pushState();
  Solver->push();

  for (SummaryCacheItem item : CalleeInSmry->getNonSymDepsCache())
    NonSymDepsCache.push_back(SummaryCacheItem(
        item.constraints, item.suffix + RenamingSuffix, item.depth + 1));
  for (SummaryCacheItem item : CalleeInSmry->getSymbDepsCache())
    SymbDepsCache.push_back(SummaryCacheItem(
        item.constraints, item.suffix + RenamingSuffix, item.depth + 1));

  // mapping actual to formal
  matchFormalActual(CalleeInSmry, CS, RenamingSuffix);

  inlineReturnSymbolicSummary(Src);

  SMTSolver::SMTResultType Result = SMTSolver::SMTRT_Uncheck;
  Solver->push();
  Solver->addAll(CtrlConds.getCacheVector());
  if (ActualNode) {
    SMTExprVec Prereq = Solver->getSMTFactory().createEmptySMTExprVec();
    TSV->setPrerequisites(Solver, CS, TraceBuilder, Prereq);
    Solver->addAll(Prereq);
  }
  Result = Solver->check();

  if (Result == SMTSolver::SMTRT_Sat && canReport(Src) && Case == 0) {
    TraceBuilder.push();
    TraceBuilder.add(CS);
    TraceBuilder.add(CalleeInSmry->getTrace());
    auto Trace = TraceBuilder.snapshot();
    TraceBuilder.pop();

    tryReport(Trace);

    Solver->pop();
  } else {

    Solver->pop();

    if (Result == SMTSolver::SMTRT_Sat) {
      TraceBuilder.push();
      TraceBuilder.add(CS);
      TraceBuilder.add(CalleeInSmry->getTrace());

      if (Case == 1) {
        pushState();
        Solver->push();

        search(Src.first, nullptr, std::make_pair(Src.first, CS), Depth);

        Solver->pop();
        popState();
      } else if (Case == 0) {
        InputSummary *InSmry =
            createInputSummary(Solver->assertions(), TraceBuilder.snapshot(),
                               &Solver->getUsedFunctionArguments(), Depth);

        addCachedConstraintsToSummary(InSmry);

        DEBUG_TRACE(dbgs() << *InSmry << "\n");

        Smry->InSmrys[isa<GuardedValueFlowArgumentNode>(Src.first) &&
                              !Src.second
                          ? Src.first
                          : nullptr]
            .insert(InSmry);

        // The condition indicates if there are multiple threads that tries to
        // report it together. it is false for uaf, because we never inline from
        // arg sym for this kind of checker.
        //	        report(InSmry, CS, Src, false);
      } else {
        assert(Case == 2);
        InputSummary *InSmry =
            createInputSummary(Solver->assertions(), TraceBuilder.snapshot(),
                               &Solver->getUsedFunctionArguments(), Depth);

        addCachedConstraintsToSummary(InSmry);

        DEBUG_TRACE(dbgs() << *InSmry << "\n");

        assert(isa<GuardedValueFlowArgumentNode>(Src.first) && !Src.second);
        Smry->TaintSourceWrapperSmrys[Src.first].insert(InSmry);
      }

      TraceBuilder.pop();
    } else {
      DEBUG_TRACE(dbgs() << "\t Inlining input summary leads unsat!\n");
    }
  }
  Solver->pop();
  popState();
  return;
}

// Inlines output summaries.
// Case 0: Normal output summary (forward propagation).
// Case 1: Continue backward search after inlining (used when searching from
// input to output).
void FunctionAnalyzer::inlineCalleeOutSummary(
    const GuardedValueFlowCallSite *CS, OutputSummary *OutSmry,
    const GuardedValueFlowCallOutputNode *CallSiteOutput,
    Vulnerability::ValueSitePairType Src, unsigned Depth, int Case) {
  auto *RetNode = OutSmry->getReturnNode();
  assert(RetNode);

  if (OutSmry->getInlineDepth() >= GSAFOptions::InlineDepth) {
    assert(isa<GuardedValueFlowNode>(OutSmry->getSourceNode()) &&
           "Such summaries have been removed during initialization!");
    return;
  }

  TraceBuilder.push();
  if (isa<GuardedValueFlowNode>(OutSmry->getSourceNode()) &&
      !OutSmry->getSourceSite()) {
    TraceBuilder.add(CS);
  }

  TraceBuilder.add(OutSmry->getTrace());
  if (Case == 0 && TSV->checkNode(CallSiteOutput, TraceBuilder)) {
    DEBUG_TRACE(dbgs() << "\t " << SKIP_STR << *CallSiteOutput << "...\n");
    TraceBuilder.pop();
    return;
  }

  Solver->push();
  pushState();

  // copy to the current context and renaming for context sensitivity
  std::string RenamingSuffix = format_str("_CS%p", CS->getInstruction());

  if (isa<GuardedValueFlowNode>(OutSmry->getSourceNode()) &&
      !OutSmry->getSourceSite()) {
  } else {
    // If it does not start from an argument, the initial control deps must be
    // satisfied otherwise the output summary cannot happen
    Solver->addAll(Solver->getCtrlDeps(CallSiteOutput));
  }

  for (const SummaryCacheItem &item : OutSmry->getNonSymDepsCache())
    NonSymDepsCache.push_back(SummaryCacheItem(
        item.constraints, item.suffix + RenamingSuffix, item.depth + 1));
  for (const SummaryCacheItem &item : OutSmry->getSymbDepsCache())
    SymbDepsCache.push_back(SummaryCacheItem(
        item.constraints, item.suffix + RenamingSuffix, item.depth + 1));

  // mapping actual to formal
  matchFormalActual(OutSmry, CS, RenamingSuffix);

  // mapping callsite output and the return value of the callee
  if (RetNode) {
    SMTExpr CSOExpr = Solver->getOrInsertExpr(CallSiteOutput);
    SMTExpr RetExpr = Solver->getSMTFactory().createBitVecConst(
        detail::encodingSymbol(RetNode) + RenamingSuffix,
        CSOExpr.getBitVecSize());
    Solver->add(RetExpr == CSOExpr);
  }

  CallSiteOutputCache.add(CallSiteOutput);

  if (Case == 0) {
    bottomUpDepthFirstSearch(CallSiteOutput, nullptr, Src, Depth);
  } else {
    assert(Case == 1);

    auto *OutSmrySrc = OutSmry->getSourceNode();
    if (auto *Arg = dyn_cast<GuardedValueFlowArgumentNode>(OutSmrySrc)) {
      if (nodeOfKind<GuardedValueFlowNode::Kind::CommonArgument,
                     GuardedValueFlowNode>(OutSmrySrc)) {
        search(CS->getCommonInput(Arg->getIndex()), nullptr, Src, Depth);
      } else if (nodeOfKind<GuardedValueFlowNode::Kind::PseudoArgument,
                            GuardedValueFlowNode>(OutSmrySrc)) {
        if (auto *PseudoInput =
                CS->getPseudoInput(OutSmry->getFunction(), Arg->getIndex())) {
          search(PseudoInput, nullptr, Src, Depth);
        }
      }
    } else {
      llvm_unreachable("When searching from output to input, the source of the "
                       "output summary trace must be an argument node!");
    }
  }

  Solver->pop();
  TraceBuilder.pop();
  popState();
  return;
}
} // namespace gsaf
} // namespace lotus
