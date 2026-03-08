/*
 *
 * Author: rainoftime
 */
#include "Checker/GVFA/UseOfUninitializedVariableChecker.h"

#include <llvm/IR/Dominators.h>
#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"
#include "Checker/GVFA/CheckerUtils.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/BugTypes.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace CheckerUtils;

namespace {

bool isUninitializedSentinel(const Value *V) {
  return isa<UndefValue>(V) || isa<PoisonValue>(V);
}

bool hasDominatingStore(const AllocaInst *AI, const LoadInst *LI,
                        const DominatorTree &DT) {
  for (const User *U : AI->users()) {
    const auto *SI = dyn_cast<StoreInst>(U);
    if (!SI || SI->getFunction() != LI->getFunction()) {
      continue;
    }
    if (SI->getPointerOperand()->stripPointerCasts() != AI) {
      continue;
    }
    if (DT.dominates(SI, LI)) {
      return true;
    }
  }

  return false;
}

} // namespace

//===----------------------------------------------------------------------===//
// Source and Sink Identification
//===----------------------------------------------------------------------===//

void UseOfUninitializedVariableChecker::getSources(
    Module *M, VulnerabilitySourcesType &Sources) {
  int site_id = 1;
  DenseMap<const Function *, std::unique_ptr<DominatorTree>> DTCache;

  auto getDT = [&DTCache](const Function *F) -> DominatorTree & {
    auto &Entry = DTCache[F];
    if (!Entry) {
      Entry = std::make_unique<DominatorTree>(*const_cast<Function *>(F));
    }
    return *Entry;
  };

  forEachInstruction(M, [&Sources, &site_id, &getDT](const Instruction *I) {
    for (const Use &Operand : I->operands()) {
      const Value *Op = Operand.get();
      if (isUninitializedSentinel(Op)) {
        Sources[{Op, site_id++}] = 1;
      }
    }

    if (auto *LI = dyn_cast<LoadInst>(I)) {
      const Value *Base = LI->getPointerOperand()->stripPointerCasts();
      if (auto *AI = dyn_cast<AllocaInst>(Base)) {
        DominatorTree &DT = getDT(LI->getFunction());
        if (!hasDominatingStore(AI, LI, DT)) {
          Sources[{LI, site_id++}] = 1;
        }
      }
    }
  });
}

void UseOfUninitializedVariableChecker::getSinks(
    Module *M, VulnerabilitySinksType &Sinks) {
  forEachInstruction(M, [&Sinks](const Instruction *I) {
    auto addSink = [&Sinks](const Value *V, const Instruction *I) {
      if (!V || isa<Function>(V)) {
        return;
      }
      Sinks[V].insert(I);
    };

    if (isa<BinaryOperator>(I) || isa<UnaryOperator>(I)) {
      for (const Use &Operand : I->operands()) {
        addSink(Operand.get(), I);
      }
    } else if (auto *Cmp = dyn_cast<CmpInst>(I)) {
      addSink(Cmp->getOperand(0), I);
      addSink(Cmp->getOperand(1), I);
    } else if (auto *BI = dyn_cast<BranchInst>(I)) {
      if (BI->isConditional()) {
        addSink(BI->getCondition(), I);
      }
    } else if (auto *SI = dyn_cast<SwitchInst>(I)) {
      addSink(SI->getCondition(), I);
    } else if (auto *Sel = dyn_cast<SelectInst>(I)) {
      addSink(Sel->getCondition(), I);
      addSink(Sel->getTrueValue(), I);
      addSink(Sel->getFalseValue(), I);
    } else if (auto *RI = dyn_cast<ReturnInst>(I)) {
      if (RI->getReturnValue()) {
        addSink(RI->getReturnValue(), I);
      }
    } else if (auto *CI = dyn_cast<CallInst>(I)) {
      for (unsigned i = 0; i < CI->arg_size(); ++i) {
        addSink(CI->getArgOperand(i), I);
      }
    } else if (auto *SI = dyn_cast<StoreInst>(I)) {
      addSink(SI->getValueOperand(), I);
    }
  });
}

bool UseOfUninitializedVariableChecker::isValidTransfer(const Value * /*From*/,
                                                        const Value *To) const {
  if (auto *CI = dyn_cast<CallInst>(To)) {
    if (auto *F = CI->getCalledFunction()) {
      if (isInitializationFunction(F->getName())) {
        return false; // Sanitized
      }
    }
  }
  return true;
}

int UseOfUninitializedVariableChecker::registerBugType() {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  return mgr.register_bug_type("Use of Uninitialized Variable",
                               BugDescription::BI_HIGH,
                               BugDescription::BC_SECURITY, "CWE-457");
}

void UseOfUninitializedVariableChecker::reportVulnerability(
    int bugTypeId, const ValueSitePairType &SourceSite, const Value *Sink,
    const std::set<const Value *> &SinkInsts,
    const std::vector<const Value *> *WitnessPath) {
  const Value *Source = SourceSite.first;

  BugReport *report = new BugReport(bugTypeId);
  int trace_level = 0;

  // Source step
  if (auto *SourceInst = dyn_cast<Instruction>(Source)) {
    std::string desc = "Uninitialized value originates here";
    std::string access = "source";
    std::vector<NodeTag> tags;

    if (isa<AllocaInst>(SourceInst)) {
      desc = "Local variable allocated without initialization";
      access = "alloca";
    } else if (isa<LoadInst>(SourceInst)) {
      desc = "Load from uninitialized memory";
      access = "load";
    }
    report->append_step(const_cast<Instruction *>(SourceInst), desc,
                        trace_level, tags, access);
    trace_level++;
  }

  // Propagation path
  if (GVFA && Sink) {
    try {
      std::vector<const Value *> witnessPath =
          WitnessPath ? *WitnessPath : GVFA->getWitnessPath(Source, Sink);
      if (witnessPath.size() > 2) {
        for (size_t i = 1; i + 1 < witnessPath.size(); ++i) {
          const Value *V = witnessPath[i];
          if (!V || !isa<Instruction>(V))
            continue;
          const Instruction *I = cast<Instruction>(V);

          std::vector<NodeTag> tags;
          if (isa<CallInst>(I)) {
            tags.push_back(NodeTag::CALL_SITE);
            trace_level++;
          }
          report->append_step(const_cast<Instruction *>(I),
                              "Potentially uninitialized value propagates",
                              trace_level, tags, "propagation");
        }
      }
    } catch (...) {
    }
  }

  // Sink step
  for (const Value *SI : SinkInsts) {
    if (auto *SinkInst = dyn_cast<Instruction>(SI)) {
      std::string desc = "Use of potentially uninitialized value";
      std::string access = "use";
      std::vector<NodeTag> tags;

      if (isa<ReturnInst>(SinkInst)) {
        desc = "Return of potentially uninitialized value";
        access = "return";
        tags.push_back(NodeTag::RETURN_SITE);
      } else if (isa<CallInst>(SinkInst)) {
        desc = "Potentially uninitialized value passed to function";
        access = "call";
        tags.push_back(NodeTag::CALL_SITE);
      } else if (isa<StoreInst>(SinkInst)) {
        desc = "Store of potentially uninitialized value";
        access = "store";
      }
      report->append_step(const_cast<Instruction *>(SinkInst), desc,
                          trace_level, tags, access);
    }
  }

  report->set_conf_score(75);
  report->set_suggestion("Initialize the variable before use");
  report->add_metadata("checker", "UseOfUninitializedVariableChecker");
  report->add_metadata("cwe", "CWE-457");
  BugReportMgr::get_instance().insert_report(bugTypeId, report, true);
}
