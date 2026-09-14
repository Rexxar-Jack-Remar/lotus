#include "Dataflow/WPDS/Backend/PreparedBackend.h"

namespace wpds::backend {

std::unique_ptr<PreparedBackend> prepareLegacyBackend(const Model &model,
                                                      WPDSQueryKind queryKind);

#ifdef LOTUS_ENABLE_WALI_OPENNWA
std::unique_ptr<PreparedBackend>
prepareWaliBackend(const Model &model, const WPDSBackendOptions &options,
                   WPDSQueryKind queryKind,
                   const std::vector<StackSymbolId> &preprocessEntries,
                   std::string &error);
#endif

std::unique_ptr<PreparedBackend>
prepareBackend(const Model &model, const WPDSBackendOptions &options,
               WPDSQueryKind queryKind,
               const std::vector<StackSymbolId> &preprocessEntries,
               std::string &error) {
  error.clear();
  if (options.backend == WPDSBackendKind::Legacy) {
    return prepareLegacyBackend(model, queryKind);
  }
#ifdef LOTUS_ENABLE_WALI_OPENNWA
  return prepareWaliBackend(model, options, queryKind, preprocessEntries,
                            error);
#else
  error = getWPDSBackendUnavailableReason(options.backend);
  return nullptr;
#endif
}

} // namespace wpds::backend
