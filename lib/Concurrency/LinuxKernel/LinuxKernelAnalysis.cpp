/**
 * @file LinuxKernelAnalysis.cpp
 * @brief Linux Kernel Analysis Coordinator Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Concurrency/LinuxKernel/LinuxKernelAnalysis.h"

#include <unordered_map>
#include <vector>

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace kernel {

namespace {

std::string jsonEscape(StringRef value) {
  std::string result;
  result.reserve(value.size());
  for (char character : value) {
    switch (character) {
    case '\\':
      result += "\\\\";
      break;
    case '"':
      result += "\\\"";
      break;
    case '\n':
      result += "\\n";
      break;
    case '\r':
      result += "\\r";
      break;
    case '\t':
      result += "\\t";
      break;
    default:
      result += character;
      break;
    }
  }
  return result;
}

StringRef sarifLevel(StringRef severity) {
  if (severity == "critical" || severity == "high") {
    return "error";
  }
  if (severity == "medium") {
    return "warning";
  }
  return "note";
}

std::string instructionKey(const Instruction *instruction) {
  if (instruction == nullptr || instruction->getFunction() == nullptr) {
    return "unknown";
  }
  size_t ordinal = 0;
  for (const BasicBlock &block : *instruction->getFunction()) {
    for (const Instruction &candidate : block) {
      if (&candidate == instruction) {
        return instruction->getFunction()->getName().str() + ":" +
               std::to_string(ordinal);
      }
      ++ordinal;
    }
  }
  return instruction->getFunction()->getName().str() + ":unknown";
}

} // namespace

void LinuxKernelAnalysis::runAnalysis() {
  process_model_.analyzeModule();
  execution_graph_.analyze();
  lock_analysis_.analyzeLocks();
  memory_model_.analyze();
  lifetime_analysis_.analyze();
  rcu_analysis_.analyzeRCU();
  wait_analysis_.analyzeWaits();

  results_.lock_deadlocks = lock_analysis_.findPotentialDeadlocks();
  results_.double_locks = lock_analysis_.findDoubleLocks();
  results_.unlock_without_lock = lock_analysis_.findUnlockWithoutLock();
  results_.lock_without_unlock = lock_analysis_.findLockWithoutUnlock();
  results_.lock_order_inversions = lock_analysis_.findLockOrderInversions();
  results_.lock_dependency_cycles = lock_analysis_.getDependencyCycles();
  results_.mix_raw_and_cooked = lock_analysis_.findMixRawAndCookedLocks();
  results_.sleep_in_atomic = process_model_.findSleepInAtomic();
  for (const Instruction *inst : lock_analysis_.findSleepInSpinlock()) {
    if (!llvm::is_contained(results_.sleep_in_atomic, inst)) {
      results_.sleep_in_atomic.push_back(inst);
    }
  }
  results_.irq_save_restore_issues = lock_analysis_.findIrqSaveRestoreIssues();
  results_.data_races = memory_model_.findDataRaceCandidates();
  results_.unresolved_calls.clear();
  for (const KernelOperation &op : process_model_.getAllOperations()) {
    if (op.kind == OperationKind::UNKNOWN_CALL) {
      results_.unresolved_calls.push_back(op.inst);
    }
  }

  results_.rcu_without_grace_period =
      rcu_analysis_.findReadSideWithoutGracePeriod();
  results_.rcu_conflicts = rcu_analysis_.findRCUConflicts();
  results_.rcu_double_free = rcu_analysis_.findRCUDoubleFree();
  results_.deref_after_free = rcu_analysis_.findDerefAfterFree();
  results_.rcu_unsafe_reclamation = rcu_analysis_.findUnsafeReclamation();

  results_.missing_wake_ups = wait_analysis_.findMissingWakeUps();
  results_.spurious_wake_ups = wait_analysis_.findSpuriousWakeUps();
  results_.lost_wakeups = wait_analysis_.findRaceBetweenWaitAndWake();
  results_.missing_completion = wait_analysis_.findMissingCompletion();
  results_.double_completion = wait_analysis_.findDoubleCompletion();
  results_.timer_not_deleted = wait_analysis_.findTimerNotDeleted();
  results_.timer_use_after_delete = wait_analysis_.findTimerUseAfterDelete();
  results_.timer_issues = results_.timer_use_after_delete;
  results_.use_after_free = lifetime_analysis_.findUseAfterFree();
  results_.async_lifetime_hazards =
      lifetime_analysis_.findAsyncLifetimeHazards();
  buildDiagnostics();
}

void LinuxKernelAnalysis::buildDiagnostics() {
  results_.diagnostics.clear();
  std::vector<std::string> assumptions = config_.assumptions();
  for (const std::string &path :
       process_model_.getSemanticRegistry().getLoadedFiles()) {
    assumptions.push_back("kernel-api-spec-loaded=" + path);
  }
  for (const std::string &error :
       process_model_.getSemanticRegistry().getErrors()) {
    assumptions.push_back("kernel-api-spec-error=" + error);
  }

  auto addPair =
      [&](StringRef category, StringRef severity, FindingConfidence confidence,
          const std::pair<const Instruction *, const Instruction *> &pair,
          StringRef rationale, std::vector<std::string> unresolved = {}) {
        DiagnosticFinding finding;
        finding.category = category.str();
        finding.severity = severity.str();
        finding.confidence = confidence;
        finding.primary = pair.first;
        finding.secondary = pair.second;
        finding.witness = {pair.first, pair.second};
        finding.assumptions = assumptions;
        finding.unresolved = std::move(unresolved);
        finding.rationale = rationale.str();
        finding.stable_id = finding.category + ":" +
                            instructionKey(pair.first) + ":" +
                            instructionKey(pair.second);
        results_.diagnostics.push_back(std::move(finding));
      };

  auto addSingle = [&](StringRef category, StringRef severity,
                       FindingConfidence confidence, const Instruction *inst,
                       StringRef rationale,
                       std::vector<std::string> unresolved = {}) {
    DiagnosticFinding finding;
    finding.category = category.str();
    finding.severity = severity.str();
    finding.confidence = confidence;
    finding.primary = inst;
    finding.witness = {inst};
    finding.assumptions = assumptions;
    finding.unresolved = std::move(unresolved);
    finding.rationale = rationale.str();
    finding.stable_id = finding.category + ":" + instructionKey(inst);
    results_.diagnostics.push_back(std::move(finding));
  };

  for (const auto &deadlock : results_.lock_deadlocks) {
    addPair("kernel-lock-deadlock", "critical", FindingConfidence::POSSIBLE,
            deadlock,
            "Opposite lock-class dependencies have explicit parallel "
            "execution contexts.");
  }
  for (const auto &cycle : results_.lock_dependency_cycles) {
    if (cycle.evidence.size() <= 2) {
      continue;
    }
    DiagnosticFinding finding;
    finding.category = "kernel-lock-strong-cycle";
    finding.severity = "high";
    finding.confidence = FindingConfidence::POSSIBLE;
    finding.primary = cycle.evidence.front();
    finding.secondary = cycle.evidence[1];
    finding.witness = cycle.evidence;
    finding.assumptions = assumptions;
    finding.rationale =
        "Lock classes form a lockdep-compatible strong dependency cycle.";
    finding.unresolved = {
        "cycle promotion requires all participating contexts to overlap"};
    finding.stable_id = finding.category + ":" +
                        instructionKey(finding.primary) + ":" +
                        instructionKey(finding.secondary);
    results_.diagnostics.push_back(std::move(finding));
  }
  for (const auto &race : results_.data_races) {
    addPair("kernel-data-race", "high", FindingConfidence::POSSIBLE, race,
            "Conflicting plain accesses share an object, may execute in "
            "parallel, and have no common protecting lock.",
            {"reads-from and coherence witnesses are not solved globally"});
  }
  for (const auto &missing : results_.missing_wake_ups) {
    addPair("kernel-missing-wakeup", "high", FindingConfidence::POSSIBLE,
            missing,
            "A concurrent producer changes the wait predicate but has no "
            "subsequent wake on the queue.");
  }
  for (const auto &lost : results_.lost_wakeups) {
    addPair("kernel-lost-wakeup", "high", FindingConfidence::POSSIBLE, lost,
            "The wait predicate is checked before queue enrollment, while a "
            "concurrent producer can update the predicate and wake the queue "
            "between those operations.");
  }
  for (const Instruction *inst : results_.rcu_unsafe_reclamation) {
    addSingle("kernel-rcu-unsafe-reclamation", "high",
              FindingConfidence::POSSIBLE, inst,
              "Reclamation is not dominated by a compatible updater-side "
              "grace period.");
  }
  for (const Instruction *inst : results_.double_completion) {
    addSingle("kernel-repeated-complete-all", "high", FindingConfidence::PROVED,
              inst,
              "complete_all is repeated on a dominated path without "
              "completion reinitialization.");
  }
  for (const Instruction *inst : results_.timer_use_after_delete) {
    addSingle("kernel-timer-rearm-after-shutdown", "high",
              FindingConfidence::PROVED, inst,
              "Timer rearming is dominated by timer_shutdown without a new "
              "lifetime epoch.");
  }
  for (const Instruction *inst : results_.lock_without_unlock) {
    addSingle("kernel-lock-leak", "high", FindingConfidence::POSSIBLE, inst,
              "The lock remains may-held at a reachable function exit.");
  }
  for (const Instruction *inst : results_.irq_save_restore_issues) {
    addSingle("kernel-irq-flags-mismatch", "high", FindingConfidence::PROVED,
              inst,
              "IRQ save/restore lacks a matching flags value on all exits.");
  }
  for (const Instruction *inst : results_.use_after_free) {
    addSingle("kernel-use-after-free", "critical", FindingConfidence::PROVED,
              inst,
              "A memory operation is dominated by reclamation of its "
              "underlying object.");
  }
  for (const auto &hazard : results_.async_lifetime_hazards) {
    addPair("kernel-async-lifetime", "critical", FindingConfidence::POSSIBLE,
            hazard,
            "An asynchronous callback remains live when its containing "
            "object is reclaimed without synchronous cancellation.");
  }
  for (const Instruction *inst : results_.unresolved_calls) {
    addSingle("kernel-unresolved-call", "informational",
              FindingConfidence::DEFERRED, inst,
              "The call has conservative sleep, spawn, and shared-memory "
              "effects because no complete callee summary is available.",
              {"callee set or semantic summary is unresolved"});
  }
}

void LinuxKernelAnalysis::printResults(raw_ostream &OS) const {
  OS << "========================================\n";
  OS << "Linux Kernel Analysis Results\n";
  OS << "========================================\n\n";

  OS << "Assumptions:";
  for (const std::string &assumption : config_.assumptions()) {
    OS << " " << assumption;
  }
  OS << "\n\n";

  OS << "Kernel API semantic specs:";
  for (const std::string &path :
       process_model_.getSemanticRegistry().getLoadedFiles()) {
    OS << " " << path;
  }
  OS << "\n";
  for (const std::string &error :
       process_model_.getSemanticRegistry().getErrors()) {
    OS << "Kernel API spec error: " << error << "\n";
  }
  OS << "\n";

  size_t total_ops = 0;
  const auto &counts = process_model_.getOperationKindCounts();
  for (const auto &pair : counts) {
    total_ops += pair.second;
  }

  OS << "Total kernel operations found: " << total_ops << "\n";
  OS << "Lock operations: " << getOperationCount(OperationKind::LOCK_ACQUIRE)
     << "/" << getOperationCount(OperationKind::LOCK_RELEASE) << "\n";
  OS << "RCU operations: " << getOperationCount(OperationKind::RCU_READ_LOCK)
     << "/" << getOperationCount(OperationKind::RCU_SYNC) << "\n";
  OS << "Wait operations: " << getOperationCount(OperationKind::WAIT_EVENT)
     << "/" << getOperationCount(OperationKind::WAKE_UP) << "\n\n";

  OS << "--- Lock Issues ---\n";
  OS << "Feasible deadlocks: " << results_.lock_deadlocks.size() << "\n";
  OS << "Double locks: " << results_.double_locks.size() << "\n";
  OS << "Unlock without lock: " << results_.unlock_without_lock.size() << "\n";
  OS << "Lock without unlock: " << results_.lock_without_unlock.size() << "\n";
  OS << "Lock order inversions: " << results_.lock_order_inversions.size()
     << "\n";
  OS << "Strong lock dependency cycles: "
     << results_.lock_dependency_cycles.size() << "\n";
  OS << "Plain-access data races: " << results_.data_races.size() << "\n\n";
  OS << "Direct use-after-free: " << results_.use_after_free.size() << "\n";
  OS << "Asynchronous lifetime hazards: "
     << results_.async_lifetime_hazards.size() << "\n\n";
  OS << "Unresolved calls with deferred effects: "
     << results_.unresolved_calls.size() << "\n\n";

  OS << "--- RCU Issues ---\n";
  OS << "Unsafe RCU reclamation: " << results_.rcu_unsafe_reclamation.size()
     << "\n\n";

  OS << "--- Wait/Completion Issues ---\n";
  OS << "Repeated complete_all without reinit: "
     << results_.double_completion.size() << "\n";
  OS << "Lost wakeup orderings: " << results_.lost_wakeups.size() << "\n";
  OS << "Timer rearm after shutdown: " << results_.timer_use_after_delete.size()
     << "\n";
  OS << "Structured diagnostics: " << results_.diagnostics.size() << "\n\n";

  OS << "========================================\n";
}

void LinuxKernelAnalysis::printSARIF(raw_ostream &OS) const {
  OS << "{\"version\":\"2.1.0\","
        "\"$schema\":\"https://json.schemastore.org/sarif-2.1.0.json\","
        "\"runs\":[{\"tool\":{\"driver\":{\"name\":\"Lotus Linux "
        "Kernel Concurrency Analysis\",\"informationUri\":"
        "\"https://zju-pl.github.io/lotus\"}},\"results\":[";
  for (size_t index = 0; index < results_.diagnostics.size(); ++index) {
    const DiagnosticFinding &finding = results_.diagnostics[index];
    if (index != 0) {
      OS << ',';
    }
    OS << "{\"ruleId\":\"" << jsonEscape(finding.category) << "\",\"level\":\""
       << sarifLevel(finding.severity) << "\",\"message\":{\"text\":\""
       << jsonEscape(finding.rationale) << "\"},\"partialFingerprints\":{"
       << "\"lotusStableId\":\"" << jsonEscape(finding.stable_id)
       << "\"},\"properties\":{\"severity\":\"" << jsonEscape(finding.severity)
       << "\",\"confidence\":\"";
    switch (finding.confidence) {
    case FindingConfidence::PROVED:
      OS << "proved";
      break;
    case FindingConfidence::POSSIBLE:
      OS << "possible";
      break;
    case FindingConfidence::DEFERRED:
      OS << "deferred";
      break;
    }
    OS << "\",\"assumptions\":[";
    for (size_t assumption = 0; assumption < finding.assumptions.size();
         ++assumption) {
      if (assumption != 0) {
        OS << ',';
      }
      OS << '"' << jsonEscape(finding.assumptions[assumption]) << '"';
    }
    OS << "],\"unresolved\":[";
    for (size_t unresolved = 0; unresolved < finding.unresolved.size();
         ++unresolved) {
      if (unresolved != 0) {
        OS << ',';
      }
      OS << '"' << jsonEscape(finding.unresolved[unresolved]) << '"';
    }
    OS << "]},\"locations\":[";
    for (size_t witness = 0; witness < finding.witness.size(); ++witness) {
      if (witness != 0) {
        OS << ',';
      }
      const Instruction *instruction = finding.witness[witness];
      std::string function =
          instruction != nullptr && instruction->getFunction() != nullptr
              ? instruction->getFunction()->getName().str()
              : "unknown";
      OS << "{\"logicalLocations\":[{\"name\":\"" << jsonEscape(function)
         << "\",\"kind\":\"function\"}]";
      if (instruction != nullptr && instruction->getDebugLoc()) {
        const DebugLoc &location = instruction->getDebugLoc();
        OS << ",\"physicalLocation\":{\"artifactLocation\":{\"uri\":\""
           << jsonEscape(location->getFilename())
           << "\"},\"region\":{\"startLine\":" << location.getLine()
           << ",\"startColumn\":" << location.getCol() << "}}";
      }
      OS << '}';
    }
    OS << "]}";
  }
  OS << "]}]}";
}

size_t LinuxKernelAnalysis::getOperationCount(OperationKind kind) const {
  const auto &counts = process_model_.getOperationKindCounts();
  auto it = counts.find(kind);
  return it != counts.end() ? it->second : 0;
}

size_t LinuxKernelAnalysis::getLockCount() const {
  return process_model_.getLockInfoMap().size();
}

size_t LinuxKernelAnalysis::getRCUSectionCount() const {
  return rcu_analysis_.getReadSideSections().size();
}

size_t LinuxKernelAnalysis::getWaitQueueCount() const {
  return wait_analysis_.getWaitContexts().size();
}

} // namespace kernel
