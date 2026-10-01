#pragma once

#include "Alias/InclusionBased/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/InclusionBased/TPA/PointerAnalysis/Engine/WorkList.h"

namespace tpa {
class GlobalState;
void solveParallel(GlobalState &, Memo &, ForwardWorkList,
                   SemiSparsePointerAnalysis::Config,
                   SemiSparsePointerAnalysis::Statistics &);
} // namespace tpa
