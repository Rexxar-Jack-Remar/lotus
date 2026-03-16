/*
 *
 * Author: rainoftime
 */
#include "Checker/GVFA/NullPointerChecker.h"

#include "Analysis/GVFA/GlobalValueFlowAnalysis.h"
#include "Analysis/NullPointer/ContextSensitiveNullCheckAnalysis.h"
#include "Analysis/NullPointer/ContextSensitiveNullFlowAnalysis.h"
#include "Analysis/NullPointer/NullCheckAnalysis.h"
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

void NullPointerChecker::getSources(Module *M,
                                    VulnerabilitySourcesType &Sources) {
  int site_id = 1;
  forEachInstruction(M, [&Sources, &site_id](const Instruction *I) {
    // 1. NULL constants stored to variables
    if (auto *SI = dyn_cast<StoreInst>(I)) {
      if (isa<ConstantPointerNull>(SI->getValueOperand())) {
        Sources[{SI, site_id++}] = 1;
      }
    }
    // 2. Memory allocation functions (can return NULL on failure)
    else if (auto *Call = dyn_cast<CallInst>(I)) {
      if (isMemoryAllocation(Call)) {
        Sources[{Call, site_id++}] = 1;
      }
    }
  });
}

void NullPointerChecker::getSinks(Module *M, VulnerabilitySinksType &Sinks) {
  auto addSink = [&](const Value *PtrOp, const Instruction *I) {
    if (!PtrOp || !PtrOp->getType()->isPointerTy())
      return;
    if (isProvenNonNull(PtrOp, I))
      return; // Filter out proven safe pointers

    Sinks[PtrOp].insert(I);
  };

  forEachInstruction(M, [&](const Instruction *I) {
    // Direct dereferences
    if (auto *LI = dyn_cast<LoadInst>(I)) {
      addSink(LI->getPointerOperand(), I);
    } else if (auto *SI = dyn_cast<StoreInst>(I)) {
      addSink(SI->getPointerOperand(), I);
    } else if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
      addSink(GEP->getPointerOperand(), I);
    }
    // Library function calls
    else if (auto *Call = dyn_cast<CallInst>(I)) {
      if (auto *F = Call->getCalledFunction()) {
        StringRef Name = F->getName();
        for (unsigned i = 0; i < Call->arg_size(); i++) {
          if (doesLibFunctionDereferenceArg(Name, i)) {
            addSink(Call->getArgOperand(i), I);
          }
        }
      }
    }
  });
}

bool NullPointerChecker::isValidTransfer(const Value * /*From*/,
                                         const Value *To) const {
  if (auto *CI = dyn_cast<CallInst>(To)) {
    if (auto *F = CI->getCalledFunction()) {
      StringRef Name = F->getName();
      // Block flow through null check functions
      return !Name.contains("check") && !Name.contains("assert");
    }
  }
  return true;
}

bool NullPointerChecker::isProvenNonNull(const Value *Ptr,
                                         const Instruction *Inst) const {
  if (!NCA && !CSNCA)
    return false;

  auto *MutablePtr = const_cast<Value *>(Ptr);
  auto *MutableInst = const_cast<Instruction *>(Inst);

  if (NCA)
    return !NCA->mayNull(MutablePtr, MutableInst);
  if (CSNCA)
    return !CSNCA->mayNull(MutablePtr, MutableInst);
  return false;
}

//===----------------------------------------------------------------------===//
// Bug Reporting
//===----------------------------------------------------------------------===//

int NullPointerChecker::registerBugType() {
  BugReportMgr &mgr = BugReportMgr::get_instance();
  return mgr.register_bug_type("NULL Pointer Dereference",
                               BugDescription::BI_HIGH,
                               BugDescription::BC_SECURITY, "CWE-476, CWE-690");
}

void NullPointerChecker::reportVulnerability(
    int bugTypeId, const ValueSitePairType &SourceSite, const Value *Sink,
    const std::set<const Value *> &SinkInsts,
    const std::vector<const Value *> *WitnessPath) {
  const Value *Source = SourceSite.first;

  BugReport *report = new BugReport(bugTypeId);
  int trace_level = 0;

  // Source step
  if (auto *SourceInst = dyn_cast<Instruction>(Source)) {
    std::vector<NodeTag> tags;
    if (isa<CallInst>(SourceInst)) {
      tags.push_back(NodeTag::CALL_SITE);
    } else if (isa<StoreInst>(SourceInst)) {
      tags.push_back(NodeTag::NONE); // Store operation
    }
    report->append_step(const_cast<Instruction *>(SourceInst),
                        "Null value originates here", trace_level, tags,
                        "source");
    trace_level++;
  } else {
    if (!SinkInsts.empty()) {
      if (auto *FirstSinkInst = dyn_cast<Instruction>(*SinkInsts.begin())) {
        std::string sourceDesc = "Null value source: ";
        llvm::raw_string_ostream OS(sourceDesc);
        Source->print(OS);
        report->append_step(const_cast<Instruction *>(FirstSinkInst), OS.str(),
                            trace_level, {}, "source");
        trace_level++;
      }
    }
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
            desc = "Null value stored to memory";
            access = "store";
          } else if (isa<LoadInst>(I)) {
            desc = "Potentially null value loaded from memory";
            access = "load";
          } else if (isa<CallInst>(I)) {
            desc = "Potentially null value passed in function call";
            access = "call";
            tags.push_back(NodeTag::CALL_SITE);
            trace_level++;
          } else if (isa<ReturnInst>(I)) {
            desc = "Potentially null value returned";
            access = "return";
            tags.push_back(NodeTag::RETURN_SITE);
          } else if (isa<PHINode>(I)) {
            desc = "Potentially null value from control flow merge";
            access = "phi";
          } else if (isa<GetElementPtrInst>(I)) {
            desc = "Pointer arithmetic on potentially null value";
            access = "gep";
          } else {
            desc = "Value propagates through here";
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
      std::string sinkDesc = "Potential null pointer dereference";
      std::string access = "dereference";
      std::vector<NodeTag> tags;

      if (isa<LoadInst>(SinkInst)) {
        sinkDesc = "Load from potentially null pointer";
        access = "load";
      } else if (isa<StoreInst>(SinkInst)) {
        sinkDesc = "Store to potentially null pointer";
        access = "store";
      } else if (isa<GetElementPtrInst>(SinkInst)) {
        sinkDesc = "GEP on potentially null pointer";
        access = "gep";
      } else if (isa<CallInst>(SinkInst)) {
        sinkDesc = "Call with potentially null pointer argument";
        access = "call";
        tags.push_back(NodeTag::CALL_SITE);
      }
      report->append_step(const_cast<Instruction *>(SinkInst), sinkDesc,
                          trace_level, tags, access);
    }
  }

  int confidence = (NCA || CSNCA) ? 85 : 70;
  report->set_conf_score(confidence);
  report->set_suggestion("Add null check before dereferencing the pointer");
  report->add_metadata("checker", "NullPointerChecker");
  report->add_metadata("cwe", "CWE-476, CWE-690");
  report->add_metadata("has_null_check_analysis",
                       (NCA || CSNCA) ? "true" : "false");
  BugReportMgr::get_instance().insert_report(bugTypeId, report, true);
}
