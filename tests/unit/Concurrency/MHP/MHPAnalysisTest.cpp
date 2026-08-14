#include "MHPAnalysisTestSupport.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<ThreadFlowGraph>);
static_assert(!std::is_copy_assignable_v<ThreadFlowGraph>);

#include "Fragments/MHPBasicAndContext.inc"
#include "Fragments/MHPThreadFlowGraph.inc"
#include "Fragments/MHPForkJoinAndSynchronization.inc"
