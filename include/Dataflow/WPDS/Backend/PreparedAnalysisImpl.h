#ifndef ANALYSIS_DATAFLOW_WPDS_BACKEND_PREPAREDANALYSISIMPL_H_
#define ANALYSIS_DATAFLOW_WPDS_BACKEND_PREPAREDANALYSISIMPL_H_

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

namespace wpds::backend {

class PreparedAnalysisImpl {
public:
  virtual ~PreparedAnalysisImpl() = default;
  virtual std::unique_ptr<mono::DataFlowResult>
  solve(const std::set<llvm::Value *> &initialFacts) = 0;
  virtual std::unique_ptr<mono::DataFlowResult>
  solveContextAggregated(const std::set<llvm::Value *> &initialFacts) = 0;
  virtual const WPDSBackendStatistics &statistics() const = 0;
  virtual const std::string &lastError() const = 0;
};

} // namespace wpds::backend

#endif // ANALYSIS_DATAFLOW_WPDS_BACKEND_PREPAREDANALYSISIMPL_H_
