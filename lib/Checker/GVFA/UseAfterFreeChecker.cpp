/*
 *
 * Author: rainoftime
 */
#include "Checker/GVFA/UseAfterFreeChecker.h"

#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"
#include "Checker/GVFA/CheckerUtils.h"
#include "Checker/Report/BugReport.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/BugTypes.h"

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
  forEachInstruction(M, [&Sources](const Instruction *I) {
    if (auto *Call = dyn_cast<CallInst>(I)) {
      if (isMemoryDeallocation(Call) && Call->arg_size() > 0) {
        // Mark the freed pointer (first argument) as a source
        auto *Arg = Call->getArgOperand(0);
        Sources[{Arg, 1}] = 1;
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
              Sinks[Arg] = new std::set<const Value *>();
              Sinks[Arg]->insert(Call);
            }
          }
        }
      }
    }

    if (PtrOp) {
      Sinks[PtrOp] = new std::set<const Value *>();
      Sinks[PtrOp]->insert(I);
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

int UseAfterFreeChecker::registerBugType() {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  return mgr.register_bug_type("Use After Free", BugDescription::BI_HIGH,
                               BugDescription::BC_SECURITY, "CWE-416");
}

void UseAfterFreeChecker::reportVulnerability(
    int bugTypeId, const Value *Source, const Value *Sink,
    const std::set<const Value *> *SinkInsts) {

  BugReport *report = new BugReport(bugTypeId);
  int trace_level = 0;

  // Source step
  if (auto *SourceInst = dyn_cast<Instruction>(Source)) {
    std::vector<NodeTag> tags;
    if (isa<CallInst>(SourceInst)) {
      tags.push_back(NodeTag::CALL_SITE);
    }
    report->append_step(const_cast<Instruction *>(SourceInst),
                        "Memory freed here", trace_level, tags, "free");
    trace_level++;
  }

  // Propagation path
  if (GVFA && Sink) {
    try {
      std::vector<const Value *> witnessPath =
          GVFA->getWitnessPath(Source, Sink);
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
  if (SinkInsts) {
    for (const Value *SI : *SinkInsts) {
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
  }

  report->set_conf_score(75);
  report->set_suggestion("Ensure memory is not used after being freed, or use "
                         "a memory-safe language feature");
  report->add_metadata("checker", "UseAfterFreeChecker");
  report->add_metadata("cwe", "CWE-416");
  BugReportMgr::get_instance().insert_report(bugTypeId, report, true);
}
