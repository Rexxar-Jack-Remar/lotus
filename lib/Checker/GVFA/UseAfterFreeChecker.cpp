/*
 *
 * Author: rainoftime
 */
#include "Checker/GVFA/UseAfterFreeChecker.h"

#include "Analysis/CFG/CFGReachability.h"
#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"
#include "Checker/GVFA/CheckerUtils.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/BugTypes.h"

#include <memory>
#include <unordered_map>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace CheckerUtils;

//===----------------------------------------------------------------------===//
// Source and Sink Identification
//===----------------------------------------------------------------------===//

void UseAfterFreeChecker::getSources(Module *M,
                                     VulnerabilitySourcesType &Sources) {
  FreeSites.clear();
  int site_id = 1;
  forEachInstruction(M, [this, &Sources, &site_id](const Instruction *I) {
    if (auto *Call = dyn_cast<CallInst>(I)) {
      if (isMemoryDeallocation(Call) && Call->arg_size() > 0) {
        // Mark the freed pointer (first argument) as a source
        auto *Arg = Call->getArgOperand(0);
        ValueSitePairType SourceSite{Arg, site_id++};
        Sources[SourceSite] = 1;
        FreeSites[SourceSite] = Call;
      }
    }
  });
}

void UseAfterFreeChecker::getSinks(Module *M, VulnerabilitySinksType &Sinks) {
  forEachInstruction(M, [&Sinks](const Instruction *I) {
    const Value *PtrOp = nullptr;

    if (auto *LI = dyn_cast<LoadInst>(I)) {
      PtrOp = LI->getPointerOperand();
    } else if (auto *SI = dyn_cast<StoreInst>(I)) {
      PtrOp = SI->getPointerOperand();
    } else if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
      PtrOp = GEP->getPointerOperand();
    } else if (auto *Call = dyn_cast<CallInst>(I)) {
      if (auto *F = Call->getCalledFunction()) {
        StringRef Name = F->getName();
        if (doesLibFunctionDereferenceArg(
                Name, 0) || // Check basic dereferences first
            Name.contains("memcpy") ||
            Name.contains("memset") || Name.contains("strcpy") ||
            Name.contains("strcmp")) {

          for (unsigned i = 0; i < Call->arg_size(); ++i) {
            auto *Arg = Call->getArgOperand(i);
            if (Arg->getType()->isPointerTy()) {
              Sinks[Arg].insert(Call);
            }
          }
        }
      }
    }

    if (PtrOp) {
      Sinks[PtrOp].insert(I);
    }
  });
}

bool UseAfterFreeChecker::isValidTransfer(const Value * /*From*/,
                                          const Value *To) const {
  if (auto *CI = dyn_cast<CallInst>(To)) {
    // Block flow through memory allocation functions - pointer becomes valid
    // again
    if (isMemoryAllocation(CI)) {
      return false;
    }
  }
  return true;
}

std::set<const Value *> UseAfterFreeChecker::filterSinkInstructions(
    const ValueSitePairType &SourceSite, const Value * /*Sink*/,
    const std::set<const Value *> &SinkInsts,
    const std::vector<const Value *> & /*WitnessPath*/) const {
  auto FreeSiteIt = FreeSites.find(SourceSite);
  if (FreeSiteIt == FreeSites.end() || !FreeSiteIt->second) {
    return {};
  }

  const Instruction *FreeInst = FreeSiteIt->second;
  std::set<const Value *> Filtered;
  std::unordered_map<const Function *, std::unique_ptr<CFGReachability>>
      ReachabilityCache;

  for (const Value *SinkValue : SinkInsts) {
    const Instruction *SinkInst = dyn_cast<Instruction>(SinkValue);
    if (!SinkInst || SinkInst->getFunction() != FreeInst->getFunction()) {
      continue;
    }

    if (SinkInst->getParent() == FreeInst->getParent()) {
      if (FreeInst->comesBefore(SinkInst)) {
        Filtered.insert(SinkValue);
      }
      continue;
    }

    auto &Reachability = ReachabilityCache[FreeInst->getFunction()];
    if (!Reachability) {
      Reachability = std::make_unique<CFGReachability>(
          const_cast<Function *>(FreeInst->getFunction()));
    }

    if (Reachability->reachable(const_cast<Instruction *>(FreeInst),
                                const_cast<Instruction *>(SinkInst))) {
      Filtered.insert(SinkValue);
    }
  }

  return Filtered;
}

int UseAfterFreeChecker::registerBugType() {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  return mgr.register_bug_type("Use After Free", BugDescription::BI_HIGH,
                               BugDescription::BC_SECURITY, "CWE-416");
}

void UseAfterFreeChecker::reportVulnerability(
    int bugTypeId, const ValueSitePairType &SourceSite, const Value *Sink,
    const std::set<const Value *> &SinkInsts,
    const std::vector<const Value *> *WitnessPath) {
  const Value *Source = SourceSite.first;

  BugReport *report = new BugReport(bugTypeId);
  int trace_level = 0;

  // Source step
  auto FreeSiteIt = FreeSites.find(SourceSite);
  if (FreeSiteIt != FreeSites.end()) {
    const Instruction *FreeInst = FreeSiteIt->second;
    std::vector<NodeTag> tags;
    if (isa<CallInst>(FreeInst)) {
      tags.push_back(NodeTag::CALL_SITE);
    }
    report->append_step(const_cast<Instruction *>(FreeInst),
                        "Memory freed here", trace_level, tags, "free");
    trace_level++;
  } else if (auto *SourceInst = dyn_cast<Instruction>(Source)) {
    report->append_step(const_cast<Instruction *>(SourceInst),
                        "Freed pointer originates here", trace_level, {},
                        "source");
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
          if (!V)
            continue;
          const Instruction *I = dyn_cast<Instruction>(V);
          if (!I)
            continue;

          std::string desc;
          std::string access;
          std::vector<NodeTag> tags;

          if (isa<StoreInst>(I)) {
            desc = "Freed pointer stored to memory";
            access = "store";
          } else if (isa<LoadInst>(I)) {
            desc = "Freed pointer loaded from memory";
            access = "load";
          } else if (isa<CallInst>(I)) {
            desc = "Freed pointer passed in function call";
            access = "call";
            tags.push_back(NodeTag::CALL_SITE);
            trace_level++;
          } else if (isa<ReturnInst>(I)) {
            desc = "Freed pointer returned";
            access = "return";
            tags.push_back(NodeTag::RETURN_SITE);
          } else if (isa<PHINode>(I)) {
            desc = "Freed pointer from control flow merge";
            access = "phi";
          } else if (isa<GetElementPtrInst>(I)) {
            desc = "Pointer arithmetic on freed pointer";
            access = "gep";
          } else {
            desc = "Freed pointer propagates through here";
            access = "propagation";
          }
          report->append_step(const_cast<Instruction *>(I), desc, trace_level,
                              tags, access);
        }
      }
    } catch (...) {
    }
  }

  // Sink step
  for (const Value *SI : SinkInsts) {
    if (auto *SinkInst = dyn_cast<Instruction>(SI)) {
      std::string sinkDesc = "Use of freed memory";
      std::string access = "use";
      std::vector<NodeTag> tags;

      if (isa<LoadInst>(SinkInst)) {
        sinkDesc = "Load from freed memory";
        access = "load";
      } else if (isa<StoreInst>(SinkInst)) {
        sinkDesc = "Store to freed memory";
        access = "store";
      } else if (isa<GetElementPtrInst>(SinkInst)) {
        sinkDesc = "GEP on freed memory";
        access = "gep";
      } else if (isa<CallInst>(SinkInst)) {
        sinkDesc = "Function call with freed memory";
        access = "call";
        tags.push_back(NodeTag::CALL_SITE);
      }
      report->append_step(const_cast<Instruction *>(SinkInst), sinkDesc,
                          trace_level, tags, access);
    }
  }

  report->set_conf_score(75);
  report->set_suggestion("Ensure memory is not used after being freed, or use "
                         "a memory-safe language feature");
  report->add_metadata("checker", "UseAfterFreeChecker");
  report->add_metadata("cwe", "CWE-416");
  BugReportMgr::get_instance().insert_report(bugTypeId, report, true);
}
