#include "Dataflow/WPDS/PreparedAnalysis.h"

#include "Dataflow/Mono/Support/Result.h"
#include "Dataflow/WPDS/Backend/PreparedAnalysisImpl.h"

namespace wpds {

PreparedAnalysis::PreparedAnalysis(
    std::unique_ptr<backend::PreparedAnalysisImpl> implementation)
    : implementation(std::move(implementation)) {}

PreparedAnalysis::PreparedAnalysis(PreparedAnalysis &&) noexcept = default;

PreparedAnalysis &
PreparedAnalysis::operator=(PreparedAnalysis &&) noexcept = default;

PreparedAnalysis::~PreparedAnalysis() = default;

std::unique_ptr<mono::DataFlowResult>
PreparedAnalysis::solve(const std::set<llvm::Value *> &initialFacts) {
  return implementation->solve(initialFacts);
}

std::unique_ptr<mono::DataFlowResult> PreparedAnalysis::solveContextAggregated(
    const std::set<llvm::Value *> &initialFacts) {
  return implementation->solveContextAggregated(initialFacts);
}

const WPDSBackendStatistics &PreparedAnalysis::getStatistics() const {
  return implementation->statistics();
}

const std::string &PreparedAnalysis::getLastError() const {
  return implementation->lastError();
}

} // namespace wpds
