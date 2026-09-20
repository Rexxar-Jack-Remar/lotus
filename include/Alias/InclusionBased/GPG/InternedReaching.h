#pragma once

#include "Alias/InclusionBased/GPG/Graph.h"

namespace lotus::gpg {

ReachingPair analyzeReachingInterned(const GPG &graph,
                                     const TypeCompatibility &compatible,
                                     unsigned k_limit);

} // namespace lotus::gpg
