//===- LeakChecker.cpp -- Memory leak detector ------------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/LeakChecker.h"

#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/BugTypes.h"
#include "Checker/Saber/SaberCheckerAPI.h"
#include "Checker/Saber/SaberOptions.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <cassert>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::analysis;

static void appendPathConditionEvents(BugReport *report,
                                      const ProgSlice *slice) {
  if (!report || !slice)
    return;
  ProgSlice::EventStack events;
  slice->evalFinalCond2Event(events);
  for (const auto &e : events) {
    if (!e.first)
      continue;
    const std::vector<NodeTag> tags = {e.second ? NodeTag::CONDITION_TRUE
                                                : NodeTag::CONDITION_FALSE};
    report->append_step(const_cast<Instruction *>(e.first), "Path condition", 0,
                        tags);
  }
}

void LeakChecker::initSrcs() {
  if (!module_ || !svfg)
    return;

  CSWorkList worklist;
  SVFGNodeBS visited;

  for (auto &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *CI = dyn_cast<CallBase>(&I)) {
          if (!CI->getType()->isPointerTy())
            continue;
          bool sourceLike = false;
          if (llvm::Function *Callee = CI->getCalledFunction()) {
            sourceLike = isSourceLikeFun(Callee->getName().str());
          } else {
            for (const llvm::Function *c : memSSA.getIndirectCallTargets(CI)) {
              if (c && isSourceLikeFun(c->getName().str())) {
                sourceLike = true;
                break;
              }
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

void LeakChecker::initSnks() {
  if (!module_ || !svfg)
    return;

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
            for (const llvm::Function *c : memSSA.getIndirectCallTargets(CI)) {
              if (c && isSinkLikeFun(c->getName().str())) {
                sinkLike = true;
                break;
              }
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

void LeakChecker::reportBug(ProgSlice *slice) {
  const SVFGNode *source = slice->getSource();
  if (!source)
    return;

  // Match SVF: only report when there is a leak (never free or partial leak).
  const bool allReachable = isAllPathReachable();
  const bool someReachable = isSomePathReachable();
  if (allReachable && someReachable)
    return; // No leak: all paths reach a free.

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const bool neverFree = (!allReachable && !someReachable);
  int bugTypeId = mgr.register_bug_type(
      neverFree ? "Memory Leak" : "Memory Leak 2", BugDescription::BI_HIGH,
      BugDescription::BC_PERFORMANCE, "CWE-401");

  BugReport *report = new BugReport(bugTypeId);

  if (const Instruction *inst = source->getInstruction()) {
    std::string tip = !someReachable
                          ? "Memory allocated here is never freed"
                          : "Memory may leak on some paths (partial leak)";
    report->append_step(const_cast<Instruction *>(inst), tip);

    if (const DebugLoc &DL = inst->getDebugLoc()) {
      if (MDNode *N = inst->getMetadata("dbg")) {
        DILocation *Loc = N ? dyn_cast<DILocation>(N) : nullptr;
        if (Loc) {
          report->get_steps().back()->src_file = Loc->getFilename().str();
          report->get_steps().back()->src_line = Loc->getLine();
          report->get_steps().back()->src_column = Loc->getColumn();
        }
      }
    }
  }
  if (!neverFree)
    appendPathConditionEvents(report, slice);

  for (auto it = slice->sinksBegin(), et = slice->sinksEnd(); it != et; ++it) {
    const SVFGNode *snk = *it;
    if (const Instruction *inst = snk->getInstruction()) {
      std::string tip = "Memory deallocated here";
      report->append_step(const_cast<Instruction *>(inst), tip, 1);
    }
  }

  mgr.insert_report(bugTypeId, report, false);

  if (SaberOptions::validateTests())
    testsValidation(slice);

  outs() << "Memory Leak detected at ";
  if (const Instruction *inst = source->getInstruction()) {
    if (const Function *F = inst->getFunction()) {
      outs() << F->getName();
    }
  }
  outs() << "\n";
}

void LeakChecker::testsValidation(const ProgSlice *slice) {
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
  validateSuccessTests(source, funName);
  validateExpectedFailureTests(source, funName);
}

void LeakChecker::validateSuccessTests(const SVFGNode *source,
                                       const std::string &fun) {
  bool success = false;
  if (fun == "SAFEMALLOC") {
    success = (isAllPathReachable() && isSomePathReachable());
  } else if (fun == "NFRMALLOC") {
    success = (!isAllPathReachable() && !isSomePathReachable());
  } else if (fun == "PLKMALLOC") {
    success = (!isAllPathReachable() && isSomePathReachable());
  } else if (fun == "CLKMALLOC") {
    success = (!isAllPathReachable() && !isSomePathReachable());
  } else if (fun == "NFRLEAKFP" || fun == "PLKLEAKFP" || fun == "LEAKFN") {
    return;
  } else {
    errs() << "SABER validation skipped: unknown test function " << fun << "\n";
    return;
  }

  const std::string srcFun = source && source->getFunction()
                                 ? source->getFunction()->getName().str()
                                 : std::string("<unknown>");
  if (success) {
    outs() << "\t SUCCESS :" << srcFun << " (src id:" << source->getId()
           << ")\n";
    return;
  }
  errs() << "\t FAILURE :" << srcFun << " (src id:" << source->getId() << ")\n";
  assert(false && "SABER leak validation failed");
}

void LeakChecker::validateExpectedFailureTests(const SVFGNode *source,
                                               const std::string &fun) {
  bool expectedFailure = false;
  if (fun == "NFRLEAKFP") {
    expectedFailure = (!isAllPathReachable() && !isSomePathReachable());
  } else if (fun == "PLKLEAKFP") {
    expectedFailure = (!isAllPathReachable() && isSomePathReachable());
  } else if (fun == "LEAKFN") {
    expectedFailure = (isAllPathReachable() && isSomePathReachable());
  } else if (fun == "SAFEMALLOC" || fun == "NFRMALLOC" || fun == "PLKMALLOC" ||
             fun == "CLKLEAKFN") {
    return;
  } else {
    errs() << "SABER validation skipped: unknown test function " << fun << "\n";
    return;
  }

  const std::string srcFun = source && source->getFunction()
                                 ? source->getFunction()->getName().str()
                                 : std::string("<unknown>");
  if (expectedFailure) {
    outs() << "\t EXPECTED-FAILURE :" << srcFun
           << " (src id:" << source->getId() << ")\n";
    return;
  }
  errs() << "\t UNEXPECTED FAILURE :" << srcFun
         << " (src id:" << source->getId() << ")\n";
  assert(false && "SABER leak unexpected validation result");
}
