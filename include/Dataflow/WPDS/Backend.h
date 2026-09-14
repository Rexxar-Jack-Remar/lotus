#ifndef ANALYSIS_DATAFLOW_WPDS_BACKEND_H_
#define ANALYSIS_DATAFLOW_WPDS_BACKEND_H_

#include <cstddef>
#include <optional>
#include <string>

namespace wpds {

enum class WPDSBackendKind { Legacy, WaliFWPDS, WaliSWPDS };
enum class WPDSQueryKind { PreStar, PostStar };
enum class WPDSObservationKind { ExactStack, StackPrefix };

struct WPDSBackendOptions {
  WPDSBackendKind backend = WPDSBackendKind::Legacy;
  bool verifyAgainstLegacy = false;
  bool collectStatistics = false;
};

struct WPDSBackendStatistics {
  WPDSBackendKind selectedBackend = WPDSBackendKind::Legacy;
  WPDSBackendKind effectiveBackend = WPDSBackendKind::Legacy;
  WPDSQueryKind query = WPDSQueryKind::PostStar;
  std::size_t controlStateCount = 0;
  std::size_t stackSymbolCount = 0;
  std::size_t popRuleCount = 0;
  std::size_t replaceRuleCount = 0;
  std::size_t pushRuleCount = 0;
  std::size_t preparationCount = 0;
  std::size_t queryCount = 0;
  double loweringMilliseconds = 0.0;
  double preparationMilliseconds = 0.0;
  double solvingMilliseconds = 0.0;
  double decodingMilliseconds = 0.0;
};

const char *toString(WPDSBackendKind backend);
const char *toString(WPDSQueryKind query);
const char *toString(WPDSObservationKind observation);
std::optional<WPDSBackendKind> parseWPDSBackend(const std::string &name);
bool isWPDSBackendAvailable(WPDSBackendKind backend);
std::string getWPDSBackendUnavailableReason(WPDSBackendKind backend);

} // namespace wpds

#endif // ANALYSIS_DATAFLOW_WPDS_BACKEND_H_
