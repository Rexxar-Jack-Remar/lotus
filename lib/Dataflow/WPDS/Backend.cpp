#include "Dataflow/WPDS/Backend.h"

namespace wpds {

const char *toString(WPDSBackendKind backend) {
  switch (backend) {
  case WPDSBackendKind::Legacy:
    return "legacy";
  case WPDSBackendKind::WaliFWPDS:
    return "wali-fwpds";
  case WPDSBackendKind::WaliSWPDS:
    return "wali-swpds";
  }
  return "unknown";
}

const char *toString(WPDSQueryKind query) {
  switch (query) {
  case WPDSQueryKind::PreStar:
    return "prestar";
  case WPDSQueryKind::PostStar:
    return "poststar";
  }
  return "unknown";
}

const char *toString(WPDSObservationKind observation) {
  switch (observation) {
  case WPDSObservationKind::ExactStack:
    return "exact-stack";
  case WPDSObservationKind::StackPrefix:
    return "stack-prefix";
  }
  return "unknown";
}

std::optional<WPDSBackendKind> parseWPDSBackend(const std::string &name) {
  if (name == "legacy") {
    return WPDSBackendKind::Legacy;
  }
  if (name == "wali-fwpds") {
    return WPDSBackendKind::WaliFWPDS;
  }
  if (name == "wali-swpds") {
    return WPDSBackendKind::WaliSWPDS;
  }
  return std::nullopt;
}

bool isWPDSBackendAvailable(WPDSBackendKind backend) {
  if (backend == WPDSBackendKind::Legacy) {
    return true;
  }
#ifdef LOTUS_ENABLE_WALI_OPENNWA
  return true;
#else
  return false;
#endif
}

std::string getWPDSBackendUnavailableReason(WPDSBackendKind backend) {
  if (isWPDSBackendAvailable(backend)) {
    return {};
  }
  return std::string("WPDS backend '") + toString(backend) +
         "' was not built; configure Lotus with "
         "-DLOTUS_ENABLE_WALI_OPENNWA=ON";
}

} // namespace wpds
