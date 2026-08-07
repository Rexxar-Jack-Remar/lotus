#ifndef NPA_NEWTON_LINEAR_ADAPTIVE_PLAN_H
#define NPA_NEWTON_LINEAR_ADAPTIVE_PLAN_H

#include <vector>

namespace npa {
namespace detail {

enum class SccBackend {
  Direct,
  Worklist,
  Tensor,
};

enum class TensorFallbackReason {
  None,
  TensorUnavailable,
  TensorNotPaperAdmissible,
  TensorLawsNotValidated,
  ProjectionFragmentUnsupported,
};

struct LinearSccExecution {
  SccBackend backend = SccBackend::Worklist;
  bool tensor_available = false;
  bool tensor_admissible = false;
  bool tensor_laws_validated = false;
  bool tensor_projection_sensitive = false;
  bool tensor_projection_fragment_supported = false;
  bool tensor_eligible = false;
  bool tensor_fallback = false;
  TensorFallbackReason tensor_fallback_reason = TensorFallbackReason::None;
};

struct LinearExecutionPlan {
  std::vector<LinearSccExecution> sccs;
};

} // namespace detail
} // namespace npa

#endif // NPA_NEWTON_LINEAR_ADAPTIVE_PLAN_H
