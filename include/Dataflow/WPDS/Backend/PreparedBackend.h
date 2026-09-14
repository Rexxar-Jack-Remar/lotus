#ifndef LOTUS_LIB_DATAFLOW_WPDS_BACKEND_PREPAREDBACKEND_H_
#define LOTUS_LIB_DATAFLOW_WPDS_BACKEND_PREPAREDBACKEND_H_

#include "Model.h"

#include <memory>
#include <string>

namespace wpds {
template <typename T> class CA;
class GenKillTransformer;
} // namespace wpds

namespace wpds::backend {

class PreparedBackend {
public:
  virtual ~PreparedBackend() = default;

  virtual bool solve(const Query &query, QueryResult &result,
                     std::string &error) = 0;
  virtual const WPDSBackendStatistics &statistics() const = 0;

  virtual const wpds::CA<GenKillTransformer> *legacyResultAutomaton() const {
    return nullptr;
  }
  virtual unsigned long legacyKeyForSymbol(StackSymbolId) const { return 0; }
};

std::unique_ptr<PreparedBackend>
prepareBackend(const Model &model, const WPDSBackendOptions &options,
               WPDSQueryKind queryKind,
               const std::vector<StackSymbolId> &preprocessEntries,
               std::string &error);

} // namespace wpds::backend

#endif // LOTUS_LIB_DATAFLOW_WPDS_BACKEND_PREPAREDBACKEND_H_
