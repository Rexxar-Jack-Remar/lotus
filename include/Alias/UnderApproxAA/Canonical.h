/**
 * @file Canonical.h
 * @brief Pointer canonicalization and equivalence checking utilities
 *
 * This file provides helper functions for normalizing pointer values and
 * detecting equivalence patterns. These utilities are used by the must-alias
 * analysis to identify when two pointers are guaranteed to refer to the same
 * memory location, despite syntactic differences in the IR.
 */

#ifndef UNDERAPPROX_CANONICAL_H
#define UNDERAPPROX_CANONICAL_H

#include <llvm/IR/Operator.h>

namespace llvm {
class DataLayout;
class Value;
} // namespace llvm

namespace UnderApprox {

/**
 * @brief Strip all no-op casts and invariant group intrinsics from a pointer
 *
 * Recursively removes operations that don't change the runtime address:
 * - Bitcasts (type changes without address changes)
 * - No-op address space casts
 * - Invariant group intrinsics (optimization hints)
 *
 * The result is a "canonical" form that can be used for comparison. This is
 * safe because these operations preserve the memory address - they only change
 * type metadata or optimization hints.
 *
 * @param V The pointer value to canonicalize
 * @return The canonical form of V (with no-op casts removed)
 *
 * Examples:
 *   - bitcast %p to i8*  →  %p (if cast is no-op)
 *   - launder_invariant_group(%p)  →  %p
 */
const llvm::Value *stripNoopCasts(const llvm::Value *V);

/**
 * @brief Check if two pointers have the same base and identical constant offsets
 *
 * Uses LLVM's stripAndAccumulateInBoundsConstantOffsets to decompose each
 * pointer into base + offset. Two pointers must-alias if they have the same
 * base and identical offsets.
 *
 * @param DL The DataLayout for pointer size calculations
 * @param A First pointer value
 * @param B Second pointer value
 * @return true if A and B have the same base and constant offset
 *
 * Examples:
 *   - GEP(%base, 0, 5) and GEP(%base, 0, 5)  →  true
 *   - GEP(%base, 0, 5) and GEP(%base, 0, 6)  →  false
 *   - %base and GEP(%base, 0, 0)  →  true
 */
bool sameConstOffset(const llvm::DataLayout &DL,
                     const llvm::Value *A, const llvm::Value *B);

/**
 * @brief Check if a GEP has all zero indices
 *
 * A GEP with all zero indices is equivalent to its base pointer.
 *
 * @param V The value to check (should be a GEPOperator)
 * @return true if V is a GEP with all zero indices, false otherwise
 *
 * Examples:
 *   - GEP(%p, 0, 0)  →  true
 *   - GEP(%p, 0)     →  true
 *   - GEP(%p, 0, 1)  →  false
 */
bool isZeroGEP(const llvm::Value *V);

/**
 * @brief Check if two values form a round-trip cast: inttoptr(ptrtoint(X))
 *
 * A pointer converted to an integer and back (with no arithmetic) is
 * guaranteed to be the same pointer. This pattern can occur in optimization
 * or when working with pointer arithmetic.
 *
 * @param A First value (should be IntToPtrInst)
 * @param B Second value (should be PtrToIntInst)
 * @return true if A and B form a round-trip cast, false otherwise
 *
 * Note: Checks both directions (A→B and B→A) since argument order may vary.
 *
 * Example:
 *   %i = ptrtoint %p to i64
 *   %q = inttoptr %i to i8*
 *   isRoundTripCast(%q, %i)  →  true (if they form a round-trip)
 */
bool isRoundTripCast(const llvm::Value *A, const llvm::Value *B);

/**
 * @brief Check if an address space cast is a no-op (same source and dest space)
 *
 * Address space casts typically change the address space. However, if the
 * source and destination spaces are the same, the cast is a no-op.
 *
 * @param V The value to check (should be an AddrSpaceCastInst)
 * @return true if the cast is a no-op, false otherwise
 *
 * Note: In well-formed LLVM IR, address space casts should always change
 * the address space. However, intermediate optimization passes may create
 * no-op casts that should be canonicalized away.
 */
bool isNoopAddrSpaceCast(const llvm::Value *V);

} // end namespace UnderApprox
#endif