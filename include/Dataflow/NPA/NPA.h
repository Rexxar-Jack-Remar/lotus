/**********************************************************************
 * Newtonian Program Analysis (NPA) – generic C++14 header
 *
 * Implements Newton-style program analysis over ω-continuous semirings:
 * - Kleene iteration: κ^(i+1) = f(κ^(i)).
 * - Newton iteration: ν^(i+1) = ν^(i) ⊔ Δ^(i), where Δ^(i) is the least
 *   solution of the \e linearized system Df|ν^(i)(X) + δ^(i) = X.
 *
 * The linearized system is an LCFL equation system when extend (⊗) is
 * non-commutative; it can be solved by SCC decomposition with local worklists,
 * or by tensor-product regularization (paired semiring → left-linear →
 * project back).
 *
 * References:
 * - Esparza et al., "Newtonian Program Analysis" (JACM): differential Df|ν,
 *   Newton sequence, convergence to least fixed point.
 * - Reps et al., "Newtonian Program Analysis via Tensor Product" (TOPLAS
 *   2016): LCFL sub-problems, regularization via tensor product (Alg. 3.4).
 *
 * Based on OCaml NPA-PMA by Di Wang.
 *
 * Implementation split:
 *   - Core/Base/Foundation.h       : public NPA types + domain helpers
 *   - Core/Base/Runtime.h          : runtime bookkeeping and errors
 *   - Core/IR/Expressions.h        : Exp0 (polynomial) / Exp1 (linearized) AST
 *   - Core/IR/Eval.h               : I0 (Exp0) / I1 (Exp1) evaluators
 *   - Core/IR/Diff.h               : ordinary and tensor differentials
 *   - Core/IR/LCFLDetector.h       : detect LCFL structure (Concat/Star)
 *   - Solver/Fixpoint.h            : fix / fix_vec (Kleene-like iteration)
 *   - Solver/LinearSolvers.h       : SCC-based linear solvers
 *   - Solver/TensorLinearSolve.h   : tensor-product solver (Alg. 3.4)
 *   - Solver/Solver.h              : KleeneIter / NewtonIter, Solver<D,ITER>
 *********************************************************************/
#ifndef NPA_HPP
#define NPA_HPP

#include "Dataflow/NPA/Solver/Solver.h"

#endif /* NPA_HPP */
