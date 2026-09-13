#pragma once

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/AffineTransfer.h"
#include "Dataflow/APA/Domains/AffineRelationDomain.h"
#include "Dataflow/APA/LLVM/ForwardProblem.h"

namespace elimination {

// Intraprocedural affine-relation (affine-equalities) analysis on the
// path-expression elimination solver. Guarded relations need not distribute
// over affine hull. The client restricts EAN to prefix factorization and uses
// input-sensitive interpretation, including when memoization is requested.
using AffineFact = AffineRelationDomain::value_type;
struct AffineEqualitiesResult
    : DataFlowResultT<llvm::Instruction *, AffineFact, AffineEdgeTransfer>,
      AffineResultContext {};

AffineEqualitiesResult
runIntraElimAffineEqualities(llvm::Function *F, EliminationOptions Opts = {});

} // namespace elimination
