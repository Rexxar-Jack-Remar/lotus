#ifndef CONCURRENCY_BUG_REPORT_H
#define CONCURRENCY_BUG_REPORT_H

#include "Checker/Report/BugTypes.h"

#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/Instructions.h>

namespace concurrency {

enum class ConcurrencyBugType {
  DATA_RACE,
  DEADLOCK,
  ATOMICITY_VIOLATION,
  LOCK_MISMATCH,
  COND_VAR_MISUSE,
  OPENMP_TASKGROUP_MISMATCH,
  OPENMP_ATOMIC_MISMATCH,
  OPENMP_PARTIAL_SYNC,
  MPI_ORPHANED_REQUEST,
  MPI_DEADLOCK,
  MPI_COLLECTIVE_MISMATCH,
  MPI_CONDITIONAL_COLLECTIVE,
  MPI_UNSYNC_RMA,
  MPI_RMA_RACE,
  MPI_WINDOW_LEAK
};

struct ConcurrencyBugStep {
  const llvm::Instruction *instruction;
  std::string description;

  ConcurrencyBugStep(const llvm::Instruction *inst, const std::string &desc)
      : instruction(inst), description(desc) {}
};

/**
 * Data-race-specific annotation for witness/SARIF (Ultimate-style).
 * Access path and read/write flag for each conflicting access.
 */
struct DataRaceInfo {
  std::string accessPath1;
  std::string accessPath2;
  bool write1 = false;
  bool write2 = false;
  std::string sharedLocation; // optional abstract location description
};

struct ConcurrencyBugReport {
  ConcurrencyBugType bugType;
  std::vector<ConcurrencyBugStep> steps;
  std::string description;
  BugDescription::BugImportance importance;
  BugDescription::BugClassification classification;

  std::unique_ptr<DataRaceInfo> dataRaceInfo;

  ConcurrencyBugReport(
      ConcurrencyBugType type, const std::string &desc,
      BugDescription::BugImportance imp = BugDescription::BI_HIGH,
      BugDescription::BugClassification cls = BugDescription::BC_ERROR)
      : bugType(type), description(desc), importance(imp), classification(cls) {
  }

  ConcurrencyBugReport(const ConcurrencyBugReport &other)
      : bugType(other.bugType), steps(other.steps),
        description(other.description), importance(other.importance),
        classification(other.classification) {
    if (other.dataRaceInfo)
      dataRaceInfo = std::make_unique<DataRaceInfo>(*other.dataRaceInfo);
  }
  ConcurrencyBugReport(ConcurrencyBugReport &&) noexcept = default;
  ConcurrencyBugReport &operator=(const ConcurrencyBugReport &other) {
    if (this != &other) {
      bugType = other.bugType;
      steps = other.steps;
      description = other.description;
      importance = other.importance;
      classification = other.classification;
      dataRaceInfo = other.dataRaceInfo
                         ? std::make_unique<DataRaceInfo>(*other.dataRaceInfo)
                         : nullptr;
    }
    return *this;
  }
  ConcurrencyBugReport &operator=(ConcurrencyBugReport &&) noexcept = default;

  void addStep(const llvm::Instruction *inst, const std::string &desc) {
    steps.emplace_back(inst, desc);
  }

  void setDataRaceInfo(std::string ap1, std::string ap2, bool w1, bool w2,
                       std::string shared = "") {
    dataRaceInfo = std::make_unique<DataRaceInfo>();
    dataRaceInfo->accessPath1 = std::move(ap1);
    dataRaceInfo->accessPath2 = std::move(ap2);
    dataRaceInfo->write1 = w1;
    dataRaceInfo->write2 = w2;
    dataRaceInfo->sharedLocation = std::move(shared);
  }
};

} // namespace concurrency

#endif // CONCURRENCY_BUG_REPORT_H
