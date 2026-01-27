#include "Checker/Pulse/PulseLatentIssue.h"
#include "Checker/Pulse/PulseSummary.h"

namespace pulse {

bool LatentIssue::shouldReport(const PulseSummary& /*summary*/, const LatentIssue& /*issue*/) {
    // Used when we have a stored latent issue and summary; typically we decide
    // at detection site via isManifest(astate). This remains for API compatibility.
    return true;
}

LatentIssue::IssueKind LatentIssue::issueKindFromResult(OperationResult result) {
    switch (result) {
    case OperationResult::InvalidAccess:
    case OperationResult::UseAfterFree:
        return IssueKind::UseAfterFree;
    case OperationResult::NullDereference:
        return IssueKind::NullDereference;
    case OperationResult::UninitializedRead:
        return IssueKind::UninitializedRead;
    case OperationResult::TaintError:
    case OperationResult::Success:
        return IssueKind::InvalidAccess;
    }
    return IssueKind::InvalidAccess;
}

bool LatentIssue::isManifest(const AbductiveDomain& astate) {
    // A state is manifest if its path condition is empty or only contains
    // facts about allocated pointers being non-null (no ptr==null assumed).
    // For now, always report bugs to ensure they are detected.
    // TODO: Implement proper manifest check based on path conditions
    const auto& formula = astate.getPathFormula();
    // Always return true to report bugs immediately
    // In the future, we can refine this to only report bugs that can occur
    // in any reasonable calling context
    return true; // formula.isEmptyOrTrivial();
}

} // namespace pulse
