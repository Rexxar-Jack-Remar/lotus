/**
 * @file LinuxKernelConfig.h
 * @brief Configuration assumptions for Linux kernel concurrency analysis.
 */

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace kernel {

enum class KernelPreemptionModel {
  NONE,
  VOLUNTARY,
  FULL,
  DYNAMIC,
  RT,
};

struct LinuxKernelConfig {
  std::string kernel_version;
  std::string architecture = "generic";
  KernelPreemptionModel preemption = KernelPreemptionModel::FULL;
  bool smp = true;
  bool strict_lkmm = true;
  bool assume_external_entries_parallel = false;
  bool load_default_api_specs = true;
  bool require_api_specs = true;
  std::vector<std::string> api_spec_paths;

  bool isPreemptRT() const { return preemption == KernelPreemptionModel::RT; }

  static LinuxKernelConfig withPreemptRT(bool enabled) {
    LinuxKernelConfig config;
    config.preemption =
        enabled ? KernelPreemptionModel::RT : KernelPreemptionModel::FULL;
    return config;
  }

  std::vector<std::string> assumptions() const {
    std::vector<std::string> result;
    result.push_back("arch=" + architecture);
    result.push_back(std::string("smp=") + (smp ? "on" : "off"));
    result.push_back(std::string("lkmm=") +
                     (strict_lkmm ? "strict" : "permissive"));
    result.push_back(
        std::string("external-entries=") +
        (assume_external_entries_parallel ? "parallel" : "explicit-only"));
    result.push_back(std::string("kernel-api-specs=") +
                     (load_default_api_specs ? "defaults" : "explicit-only"));
    for (const std::string &path : api_spec_paths) {
      result.push_back("kernel-api-spec=" + path);
    }
    switch (preemption) {
    case KernelPreemptionModel::NONE:
      result.push_back("preemption=none");
      break;
    case KernelPreemptionModel::VOLUNTARY:
      result.push_back("preemption=voluntary");
      break;
    case KernelPreemptionModel::FULL:
      result.push_back("preemption=full");
      break;
    case KernelPreemptionModel::DYNAMIC:
      result.push_back("preemption=dynamic");
      break;
    case KernelPreemptionModel::RT:
      result.push_back("preemption=rt");
      break;
    }
    if (!kernel_version.empty()) {
      result.push_back("kernel=" + kernel_version);
    }
    return result;
  }
};

} // namespace kernel
