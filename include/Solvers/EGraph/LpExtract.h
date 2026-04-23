#pragma once

#include "Solvers/EGraph/Extract.h"

namespace lotus::egraph {

template <typename L, typename A = NoAnalysis<L>, typename CostFn = AstSize<L>>
using LpExtractor = Extractor<L, A, CostFn>;

} // namespace lotus::egraph
