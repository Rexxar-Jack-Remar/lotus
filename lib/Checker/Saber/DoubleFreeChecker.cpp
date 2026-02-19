//===- DoubleFreeChecker.cpp -- Double free detector
//--------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/DoubleFreeChecker.h"

#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/BugTypes.h"
#include "Checker/Saber/SaberCheckerAPI.h"
#include "Checker/Saber/SaberOptions.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>
#include <cassert>

using namespace llvm;
using namespace lotus::analysis;

void DoubleFreeChecker::initSrcs() {
  if (!module_ || !svfg)
    return;

  CSWorkList worklist;
  SVFGNodeBS visited;

  // For double-free, sources are free() calls (first free)
  for (auto &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *CI = dyn_cast<CallBase>(&I)) {
          bool sourceLike = false;
          if (llvm::Function *Callee = CI->getCalledFunction()) {
            sourceLike = isSourceLikeFun(Callee->getName().str());
          } else {
            for (const llvm::Function *c : svfg->getConnectedCallees(CI))
              if (c && isSourceLikeFun(c->getName().str())) {
                sourceLike = true;
                break;
              }
          }
          if (sourceLike)
            worklist.push_back(CI);
        }
      }
    }
  }

  while (!worklist.empty()) {
    const llvm::CallBase *cs = worklist.front();
    worklist.pop_front();

    if (!cs->getCaller())
      continue;
    if (cs->getCaller()->isDeclaration())
      continue;

    SVFGNode *node = svfg->getDef(cs);
    if (!node)
      node = svfg->getValueNode(cs);
    if (!node)
      continue;
    if (visited.count(node->getId()))
      continue;
    visited.insert(node->getId());

    CallSiteSet csSet;
    if (isInAWrapper(node, csSet)) {
      for (const llvm::CallBase *c : csSet)
        worklist.push_back(c);
    } else {
      const llvm::Function *caller = cs->getCaller();
      if (!caller->isDeclaration() &&
          !SaberCheckerAPI::getCheckerAPI()->isExtCall(caller)) {
        addToSources(node);
        addSrcToCSID(node, cs);
      }
    }
  }
}

void DoubleFreeChecker::initSnks() {
  if (!module_ || !svfg)
    return;

  // For double-free, sinks are also free() calls (second free)
  for (auto &F : *module_) {
    if (F.isDeclaration())
      continue;

    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *CI = dyn_cast<CallBase>(&I)) {
          bool sinkLike = false;
          if (llvm::Function *Callee = CI->getCalledFunction()) {
            sinkLike = isSinkLikeFun(Callee->getName().str());
          } else {
            for (const llvm::Function *c : svfg->getConnectedCallees(CI))
              if (c && isSinkLikeFun(c->getName().str())) {
                sinkLike = true;
                break;
              }
          }
          if (!sinkLike)
            continue;

          const auto &actualParms = svfg->getActualParms(CI);
          unsigned argIndex = 0;
          for (auto &arg : CI->args()) {
            if (!arg->getType()->isPointerTy()) {
              ++argIndex;
              continue;
            }

            SVFGNode *actualParmNode = nullptr;
            for (SVFGNode *n : actualParms) {
              if (!n)
                continue;
              if (n->getNodeKind() != SVFGK::ActualParm)
                continue;
              auto *ap = llvm::dyn_cast<ActualParmSVFGNode>(n);
              if (!ap)
                continue;
              if (ap->getParamIndex() == argIndex) {
                actualParmNode = n;
                break;
              }
            }

            if (actualParmNode)
              addToSinks(actualParmNode);

            SVFGNode *snkNode = svfg->getValueNode(arg.get());
            if (snkNode) {
              if (!actualParmNode)
                addToSinks(snkNode);
              if (arg->getType()->getPointerElementType()->isPointerTy()) {
                for (auto &edge : snkNode->getOutEdges()) {
                  if (edge->getEdgeKind() == SVFGEdgeK::IntraLoad)
                    addToSinks(edge->getDstNode());
                }
              }
            }

            ++argIndex;
          }
        }
      }
    }
  }
}

static void appendPathConditionEvents(BugReport *report, const ProgSlice *slice) {
  if (!report || !slice)
    return;
  ProgSlice::EventStack events;
  slice->evalFinalCond2Event(events);
  for (const auto &e : events) {
    if (!e.first)
      continue;
    const std::vector<NodeTag> tags = {
        e.second ? NodeTag::CONDITION_TRUE : NodeTag::CONDITION_FALSE};
    report->append_step(const_cast<Instruction *>(e.first), "Path condition", 0,
                        tags);
  }
}

void DoubleFreeChecker::reportBug(ProgSlice *slice) {
  const SVFGNode *source = slice->getSource();
  if (!source)
    return;

  // Match SVF: only report when a double-free path exists (two free sinks
  // reachable on the same path).
  if (slice->isSatisfiableForPairs())
    return;

  BugReportMgr &mgr = BugReportMgr::get_instance();
  int bugTypeId = mgr.register_bug_type("Double Free", BugDescription::BI_HIGH,
                                        BugDescription::BC_SECURITY, "CWE-415");

  BugReport *report = new BugReport(bugTypeId);

  if (const Instruction *inst = source->getInstruction()) {
    std::string tip = "Memory freed here";
    report->append_step(const_cast<Instruction *>(inst), tip);
  }
  appendPathConditionEvents(report, slice);

  for (auto it = slice->sinksBegin(), et = slice->sinksEnd(); it != et; ++it) {
    const SVFGNode *snk = *it;
    if (const Instruction *inst = snk->getInstruction()) {
      std::string tip = "Memory freed again - double free!";
      report->append_step(const_cast<Instruction *>(inst), tip, 1);
    }
  }

  mgr.insert_report(bugTypeId, report, false);

  if (SaberOptions::validateTests())
    testsValidation(slice);

  outs() << "Double Free detected at ";
  if (const Instruction *inst = source->getInstruction()) {
    if (const Function *F = inst->getFunction()) {
      outs() << F->getName();
    }
  }
  outs() << "\n";
}

void DoubleFreeChecker::testsValidation(ProgSlice *slice) {
  const SVFGNode *source = slice ? slice->getSource() : nullptr;
  if (!source)
    return;
  const llvm::CallBase *cs = getSrcCSID(source);
  if (!cs)
    return;
  const llvm::Function *fun = cs->getCalledFunction();
  if (!fun)
    return;
  const std::string funName = fun->getName().str();
  validateSuccessTests(slice, funName);
  validateExpectedFailureTests(slice, funName);
}

void DoubleFreeChecker::validateSuccessTests(ProgSlice *slice,
                                             const std::string &fun) {
  const SVFGNode *source = slice ? slice->getSource() : nullptr;
  if (!source)
    return;

  bool success = false;
  if (fun == "SAFEMALLOC") {
    success = slice->isSatisfiableForPairs();
  } else if (fun == "DOUBLEFREEMALLOC") {
    success = !slice->isSatisfiableForPairs();
  } else if (fun == "DOUBLEFREEMALLOCFN" || fun == "SAFEMALLOCFP") {
    return;
  } else {
    errs() << "SABER validation skipped: unknown test function " << fun << "\n";
    return;
  }

  const std::string srcFun = source->getFunction()
                                 ? source->getFunction()->getName().str()
                                 : std::string("<unknown>");
  if (success) {
    outs() << "\t SUCCESS :" << srcFun << " (src id:" << source->getId()
           << ")\n";
    outs() << "\t\t double free path:\n" << slice->evalFinalCond() << "\n";
    return;
  }
  errs() << "\t FAILURE :" << srcFun << " (src id:" << source->getId()
         << ")\n";
  errs() << "\t\t double free path:\n" << slice->evalFinalCond() << "\n";
  assert(false && "SABER double-free validation failed");
}

void DoubleFreeChecker::validateExpectedFailureTests(ProgSlice *slice,
                                                     const std::string &fun) {
  const SVFGNode *source = slice ? slice->getSource() : nullptr;
  if (!source)
    return;

  bool expectedFailure = false;
  if (fun == "DOUBLEFREEMALLOCFN") {
    expectedFailure = slice->isSatisfiableForPairs();
  } else if (fun == "SAFEMALLOCFP") {
    expectedFailure = !slice->isSatisfiableForPairs();
  } else if (fun == "SAFEMALLOC" || fun == "DOUBLEFREEMALLOC") {
    return;
  } else {
    errs() << "SABER validation skipped: unknown test function " << fun << "\n";
    return;
  }

  const std::string srcFun = source->getFunction()
                                 ? source->getFunction()->getName().str()
                                 : std::string("<unknown>");
  if (expectedFailure) {
    outs() << "\t EXPECTED-FAILURE :" << srcFun << " (src id:" << source->getId()
           << ")\n";
    outs() << "\t\t double free path:\n" << slice->evalFinalCond() << "\n";
    return;
  }
  errs() << "\t UNEXPECTED FAILURE :" << srcFun << " (src id:" << source->getId()
         << ")\n";
  errs() << "\t\t double free path:\n" << slice->evalFinalCond() << "\n";
  assert(false && "SABER double-free unexpected validation result");
}
