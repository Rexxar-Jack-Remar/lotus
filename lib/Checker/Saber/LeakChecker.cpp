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
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include <llvm/IR/Constants.h>
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

static const llvm::Function *getDirectCallee(const llvm::CallBase *call) {
  if (!call)
    return nullptr;
  if (const llvm::Function *callee = call->getCalledFunction())
    return callee;
  const llvm::Value *called = call->getCalledOperand();
  if (!called)
    return nullptr;
  return llvm::dyn_cast<llvm::Function>(called->stripPointerCasts());
}

static const llvm::Value *getReportValueForNode(const SVFGNode *node) {
  if (!node)
    return nullptr;
  if (const Instruction *inst = node->getInstruction())
    return inst;
  if (const auto *actualParm = dyn_cast<ActualParmSVFGNode>(node))
    return actualParm->getCallSite();
  return nullptr;
}

static std::vector<const llvm::Function *>
collectResolvedCallees(const llvm::CallBase *call, SaberSVFGBuilder &builder) {
  std::vector<const llvm::Function *> callees;
  if (!call)
    return callees;
  if (const llvm::Function *directCallee = getDirectCallee(call)) {
    callees.push_back(directCallee);
    return callees;
  }
  return builder.getIndirectCallTargets(call);
}

using FuncSet = std::unordered_set<const Function *>;
using FuncGraph = std::unordered_map<const Function *, FuncSet>;

static bool isModuleCtorOrDtor(StringRef name) {
  return name == "llvm.global_ctors" || name == "llvm.global_dtors";
}

static void addFunctionSeedsFromGlobalArray(const GlobalVariable *gv,
                                            FuncSet &seeds) {
  if (!gv || !gv->hasInitializer())
    return;
  const Constant *init = gv->getInitializer();
  const auto *arr = dyn_cast<ConstantArray>(init);
  if (!arr)
    return;
  for (unsigned i = 0; i < arr->getNumOperands(); ++i) {
    const auto *elt = dyn_cast<ConstantStruct>(arr->getOperand(i));
    if (!elt || elt->getNumOperands() < 2)
      continue;
    const Value *op = elt->getOperand(1)->stripPointerCasts();
    if (const auto *fn = dyn_cast<Function>(op)) {
      if (!fn->isDeclaration())
        seeds.insert(fn);
    }
  }
}

static FuncSet
computeReachableFunctions(const Module *M,
                          const std::function<std::vector<const Function *>(
                              const CallBase *)> &resolveIndirectTargets) {
  FuncSet reachable;
  FuncSet seeds;
  FuncGraph graph;
  if (!M)
    return reachable;

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;
    graph.emplace(&F, FuncSet{});

    if (F.getName() == "main" || !F.hasLocalLinkage() || F.hasAddressTaken())
      seeds.insert(&F);
  }

  for (const GlobalVariable &GV : M->globals()) {
    if (!isModuleCtorOrDtor(GV.getName()))
      continue;
    addFunctionSeedsFromGlobalArray(&GV, seeds);
  }

  for (const Function &F : *M) {
    if (F.isDeclaration())
      continue;
    auto it = graph.find(&F);
    if (it == graph.end())
      continue;

    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        const auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;
        const Function *callee = getDirectCallee(CB);
        if (callee) {
          if (!callee->isDeclaration())
            it->second.insert(callee);
          continue;
        }
        for (const Function *target : resolveIndirectTargets(CB)) {
          if (target && !target->isDeclaration())
            it->second.insert(target);
        }
      }
    }
  }

  if (seeds.empty()) {
    for (const auto &kv : graph)
      seeds.insert(kv.first);
  }

  std::deque<const Function *> worklist(seeds.begin(), seeds.end());
  while (!worklist.empty()) {
    const Function *F = worklist.front();
    worklist.pop_front();
    if (!reachable.insert(F).second)
      continue;
    auto it = graph.find(F);
    if (it == graph.end())
      continue;
    for (const Function *callee : it->second) {
      if (callee && !callee->isDeclaration())
        worklist.push_back(callee);
    }
  }

  return reachable;
}

void LeakChecker::initSrcs() {
  if (!module_ || !svfg)
    return;

  // SVF parity: skip sources in dead/uncalled functions.
  const FuncSet reachableFunctions =
      computeReachableFunctions(module_, [this](const CallBase *call) {
        return memSSA.getIndirectCallTargets(call);
      });

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
          for (const llvm::Function *c : collectResolvedCallees(CI, memSSA)) {
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
      if (!caller->isDeclaration() && reachableFunctions.count(caller) &&
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
          bool multiLevelSink = false;
          for (const llvm::Function *c : collectResolvedCallees(CI, memSSA)) {
            if (!c)
              continue;
            if (isSinkLikeFun(c->getName().str())) {
              sinkLike = true;
              multiLevelSink =
                  multiLevelSink ||
                  SaberCheckerAPI::getCheckerAPI()->isMultiLevelMemDealloc(c);
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
              if (multiLevelSink &&
                  arg->getType()->getPointerElementType()->isPointerTy()) {
                for (const User *user : arg.get()->users()) {
                  const auto *load = dyn_cast<LoadInst>(user);
                  if (!load || load->getPointerOperand() != arg.get())
                    continue;
                  if (SVFGNode *loadNode = svfg->getDef(load)) {
                    addToSinks(loadNode);
                  } else if (SVFGNode *loadNode = svfg->getValueNode(load)) {
                    addToSinks(loadNode);
                  }
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
  const llvm::CallBase *sourceCall = getSrcCSID(source);
  const llvm::Value *reportSource =
      sourceCall ? static_cast<const llvm::Value *>(sourceCall)
                 : static_cast<const llvm::Value *>(source->getInstruction());

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

  if (reportSource) {
    std::string tip = !someReachable
                          ? "Memory allocated here is never freed"
                          : "Memory may leak on some paths (partial leak)";
    report->append_step(const_cast<Value *>(reportSource), tip);
  }
  if (!neverFree)
    appendPathConditionEvents(report, slice);

  for (auto it = slice->sinksBegin(), et = slice->sinksEnd(); it != et; ++it) {
    const SVFGNode *snk = *it;
    if (const Value *sinkValue = getReportValueForNode(snk)) {
      std::string tip = "Memory deallocated here";
      report->append_step(const_cast<Value *>(sinkValue), tip, 1);
    }
  }

  mgr.insert_report(bugTypeId, report, false);

  if (SaberOptions::validateTests())
    testsValidation(slice);

  outs() << "Memory Leak detected at ";
  if (const auto *inst = dyn_cast_or_null<Instruction>(reportSource)) {
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
             fun == "CLKMALLOC") {
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
