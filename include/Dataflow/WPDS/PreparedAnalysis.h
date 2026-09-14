#ifndef ANALYSIS_DATAFLOW_WPDS_PREPAREDANALYSIS_H_
#define ANALYSIS_DATAFLOW_WPDS_PREPAREDANALYSIS_H_

#include "Dataflow/WPDS/Backend.h"

#include <memory>
#include <set>
#include <string>

namespace llvm {
class Value;
} // namespace llvm

namespace mono {
class DataFlowResult;
} // namespace mono

namespace wpds {
namespace backend {
class PreparedAnalysisImpl;
} // namespace backend

class InterProceduralDataFlowEngine;

/// A direction-bound analysis session over one immutable lowered WPDS model.
/// The referenced LLVM module must outlive the session and returned results.
class PreparedAnalysis {
public:
  PreparedAnalysis(PreparedAnalysis &&) noexcept;
  PreparedAnalysis &operator=(PreparedAnalysis &&) noexcept;
  ~PreparedAnalysis();

  PreparedAnalysis(const PreparedAnalysis &) = delete;
  PreparedAnalysis &operator=(const PreparedAnalysis &) = delete;

  std::unique_ptr<mono::DataFlowResult>
  solve(const std::set<llvm::Value *> &initialFacts = {});
  std::unique_ptr<mono::DataFlowResult>
  solveContextAggregated(const std::set<llvm::Value *> &initialFacts = {});

  const WPDSBackendStatistics &getStatistics() const;
  const std::string &getLastError() const;

private:
  friend class InterProceduralDataFlowEngine;
  explicit PreparedAnalysis(
      std::unique_ptr<backend::PreparedAnalysisImpl> implementation);

  std::unique_ptr<backend::PreparedAnalysisImpl> implementation;
};

} // namespace wpds

#endif // ANALYSIS_DATAFLOW_WPDS_PREPAREDANALYSIS_H_
