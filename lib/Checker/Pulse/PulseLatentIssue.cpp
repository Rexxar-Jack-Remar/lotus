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
    case OperationResult::MemoryLeak:
        return IssueKind::MemoryLeak;
    case OperationResult::TaintError:
    case OperationResult::Success:
        return IssueKind::InvalidAccess;
    }
    return IssueKind::InvalidAccess;
}

bool LatentIssue::isManifest(const AbductiveDomain& astate) {
    // A state is manifest if its path condition is empty or only contains
    // facts about allocated pointers being non-null (no ptr==null assumed).
    const auto& formula = astate.getPathFormula();
    return formula.isEmptyOrTrivial();
}

} // namespace pulse
