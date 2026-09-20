#pragma once

#include <cstddef>

namespace lotus::gpg {

enum class AnalysisMode {
  FlowAndContextSensitive,
  FlowInsensitiveContextSensitive,
  FlowAndContextInsensitive,
};

struct GPGConfig {
  AnalysisMode mode = AnalysisMode::FlowAndContextSensitive;

  // Appendix B uses k = 3 for heap paths that are live on entry.
  unsigned heap_indirection_limit = 3;

  bool field_sensitive = true;
  bool array_index_sensitive = false;
  bool enable_blocking = true;
  bool enable_dead_gpu_elimination = true;
  bool enable_coalescing = true;
  bool enable_type_based_non_aliasing = true;

  // Section 10.3.1: retain a symbolic call when a non-trivial summary is
  // dominated by context-dependent GPUs. Zero disables this heuristic.
  std::size_t symbolic_summary_min_gpus = 10;
  double symbolic_summary_ratio = 0.80;
};

} // namespace lotus::gpg
