/*
// This file implements a must-alias analysis using union-find with congruence
// closure. The goal is to compute an *under-approximation* of pointer
// equivalence: if two pointers are in the same equivalence class, they are
// guaranteed to alias. We never produce false positives, but may miss aliases.
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ DATA STRUCTURE: Union-Find with Watches                                 │
// └─────────────────────────────────────────────────────────────────────────┘
//
//   • Each Value* gets a unique ID (IdTy = unsigned)
//   • Val2Id: DenseMap<const Value*, IdTy>  — value → ID lookup
//   • Id2Val: vector<const Value*>          — ID → value lookup
//   • Nodes:  vector<{Parent, Rank}>        — union-find forest
//   • Watches: vector<SmallVector<Inst*>>   — per-class watch lists
//
//   Watches[i] stores instructions that depend on class i. When two classes
//   merge, we revisit all watched instructions to check if new semantic rules
//   can fire (e.g., a PHI becomes "closed" when all operands unify).
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ ALGORITHM: Three-Phase Construction                                      │
// └─────────────────────────────────────────────────────────────────────────┘
//
//   Phase 1: SEED with atomic must-alias pairs
//   ──────────────────────────────────────────
//   Scan all instructions and their operands. For each pair (A, B), apply
//   local syntactic rules to check if A and B must alias:
//
//     1. Identity:           A == B (after stripping casts)
//     2. Cast equivalence:   A = bitcast(B) or A = addrspacecast(B)
//     3. Const-offset GEP:   GEP(base, C₁) == GEP(base, C₂) when C₁ ≡ C₂
//     4. Zero GEP:           GEP(p, 0, 0, ...) == p
//     5. Round-trip cast:    inttoptr(ptrtoint(p)) == p
//     6. Same object:        Both derive from same alloca/global
//     7. Constant null:      null == null (same address space)
//     8. Trivial PHI:        phi [p, bb1], [p, bb2] == p
//     9. Trivial Select:     select cond, p, p == p
//    10. Same allocation:    Same malloc/new call = same pointer
//    11. Enhanced round-trip: inttoptr(ptrtoint(p) + 0) == p
//
//   All matching pairs (A, B) are added to a worklist WL.
//   Additionally, register each pointer-producing instruction I as "watched"
//   by the equivalence classes of its pointer operands.
//
//   Phase 2: PROPAGATE with normalized pointer terms
//   ────────────────────────────────────────────────
//   Process the worklist WL:
//
//     while WL not empty:
//       pop (A, B) from WL
//       CA ← find(A);  CB ← find(B)
//       if CA == CB: continue  // already unified
//
//       unite(CA, CB) → NewRoot
//
//       // Revisit all instructions watching the merged class
//       for each instruction I in Watches[NewRoot]:
//         if I is a pointer instruction:
//           rebuild I's normalized term:
//             - closed phi/select collapse to a common class
//             - congruent geps match by normalized base/index structure
//             - matching terms yield more must-alias pairs
//
//   These term rules are *inductive*: as more values unify, previously
//   distinct terms may collapse and enable further propagation.
//
//   Phase 3: REFINE with external analyses (optional)
//   ─────────────────────────────────────────────────
//   If MemorySSA is available:
//     • Store-Load forwarding: load p after store v to the same singleton
//       slot = v (no clobber)
//
//   If DominatorTree is available:
//     • Single-store alloca forwarding when store dominates all loads
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ QUERY: mustAlias(A, B)                                                  │
// └─────────────────────────────────────────────────────────────────────────┘
//
//   After construction, query in O(α(N)) time:
//     return find(id(A)) == find(id(B))
//
//   If A and B are in the same equivalence class, they are guaranteed to
//   alias (sound under-approximation). If not, we don't know.

*/

#include "Alias/UnderApproxAA/EquivDB.h"

#include "Alias/UnderApproxAA/Canonical.h"

#include <functional>
#include <mutex>
#include <queue>
#include <unordered_map>

#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;
using namespace UnderApprox;

static uint64_t encodeSignedInt64(int64_t Value) {
  const uint64_t Bits = static_cast<uint64_t>(Value);
  return (Bits << 1) ^ static_cast<uint64_t>(Value >> 63);
}

//
//===----------------------------------------------------------------------===//
//             1.  Atomic Must-Alias Rules
//===----------------------------------------------------------------------===//
//
// Each rule below checks for a specific syntactic pattern that guarantees
// two pointers must alias. All rules are conservative: they never produce
// false positives, but may miss some true aliases (under-approximation).
//
// To add a new rule:
//   1. Create a static helper function following the pattern below
//   2. Add clear documentation explaining when the rule applies
//   3. Call it from atomicMustAlias()
//===----------------------------------------------------------------------===//

/// Rule 1: Identity
/// Two pointers are the same SSA value (after stripping no-op casts).
/// Example: %p and %p
static bool checkIdentity(const Value *S1, const Value *S2) { return S1 == S2; }

/// Rule 2: Bitcast/AddressSpaceCast Equivalence
/// A pointer and its bitcast/addrspacecast (when no-op) are aliases.
/// Example: %p and bitcast %p to i8*
static bool checkCastEquivalence(const Value *S1, const Value *S2) {
  if (auto *Op = dyn_cast<Operator>(S1))
    if ((isa<BitCastOperator>(Op) || isNoopAddrSpaceCast(Op)) &&
        stripNoopCasts(Op->getOperand(0)) == S2)
      return true;

  if (auto *Op = dyn_cast<Operator>(S2))
    if ((isa<BitCastOperator>(Op) || isNoopAddrSpaceCast(Op)) &&
        stripNoopCasts(Op->getOperand(0)) == S1)
      return true;

  return false;
}

/// Rule 3: Constant Offset GEP Equivalence
/// Two GEPs with the same base and identical constant offsets are aliases.
/// Example: GEP(%base, 0, i) and GEP(%base, 0, i)
static bool checkConstOffsetGEP(const DataLayout &DL, const Value *S1,
                                const Value *S2) {
  return sameConstOffset(DL, S1, S2);
}

/// Rule 4: Zero-Index GEP ↔ Base Pointer
/// A GEP with all zero indices is the same as its base pointer.
/// Example: GEP(%p, 0, 0) and %p
static bool checkZeroGEP(const Value *S1, const Value *S2) {
  if (isZeroGEP(S1) &&
      stripNoopCasts(cast<GEPOperator>(S1)->getPointerOperand()) == S2)
    return true;

  if (isZeroGEP(S2) &&
      stripNoopCasts(cast<GEPOperator>(S2)->getPointerOperand()) == S1)
    return true;

  return false;
}

/// Rule 4b: Same base and same index operands (including variable SSA indices)
/// Two GEPs with the same base and identical index operands must alias.
/// Example: GEP(%base, i) and GEP(%base, i)
static bool checkSameGEPOperands(const Value *S1, const Value *S2) {
  return sameGEPOperands(S1, S2);
}

/// Rule 5: Round-Trip Pointer ↔ Integer Cast
/// A pointer converted to integer and back (with no arithmetic) is unchanged.
/// Example: inttoptr(ptrtoint(%p)) and %p
static bool checkRoundTripCast(const Value *S1, const Value *S2) {
  return isRoundTripCast(S1, S2) || isRoundTripCast(S2, S1);
}

/// Rule 6: Same Underlying Object at Zero Offset
/// Two pointers that both have zero offset from the same alloca or global
/// must alias.  We require zero offset to avoid false positives: e.g.
/// GEP(%alloca, 4) and GEP(%alloca, 8) share the same underlying object
/// but point to different locations.
///
/// "Zero offset" is checked by verifying that stripping all no-op casts
/// and all-zero-index GEPs from each pointer yields the same base object.
/// This is equivalent to requiring that both pointers ARE the base object
/// (up to no-op casts / zero GEPs).
///
/// Example: bitcast(%alloca to i8*) and GEP(%alloca, 0) → true
/// Counter-example: GEP(%alloca, 4) and GEP(%alloca, 8) → false (different
/// offsets)
static bool checkSameUnderlyingObject(const Value *S1, const Value *S2) {
  const Value *U1 = getUnderlyingObject(S1);
  const Value *U2 = getUnderlyingObject(S2);

  // Only consider stack allocations and globals (not heap objects, which are
  // handled by checkSameAllocation).
  if (U1 != U2 || (!isa<AllocaInst>(U1) && !isa<GlobalVariable>(U1)))
    return false;

  // Require that both pointers reduce to the base object itself after
  // stripping no-op casts and zero-index GEPs.  If either pointer has a
  // non-zero offset (e.g. GEP with a non-zero index), stripNoopCasts will
  // stop before the base and the comparison will fail.
  return stripNoopCasts(S1) == U1 && stripNoopCasts(S2) == U2;
}

/// Rule 7: Constant Null Pointers
/// Two null pointers in the same address space are aliases.
/// Example: null and null (both in addrspace(0))
static bool checkConstantNull(const Value *S1, const Value *S2) {
  return isa<ConstantPointerNull>(S1) && isa<ConstantPointerNull>(S2) &&
         S1->getType()->getPointerAddressSpace() ==
             S2->getType()->getPointerAddressSpace();
}

/// Rule 8: Trivial PHI Node
/// A PHI where all incoming values are the same (after stripping casts)
/// is equivalent to that common value.
/// Example: phi [%p, %bb1], [%p, %bb2] and %p
static bool checkTrivialPHI(const Value *S1, const Value *S2) {
  if (auto *PN = dyn_cast<PHINode>(S1))
    if (llvm::all_of(PN->incoming_values(),
                     [&](const Value *V) { return stripNoopCasts(V) == S2; }))
      return true;

  if (auto *PN = dyn_cast<PHINode>(S2))
    if (llvm::all_of(PN->incoming_values(),
                     [&](const Value *V) { return stripNoopCasts(V) == S1; }))
      return true;

  return false;
}

/// Rule 9: Trivial Select
/// A select where both branches produce the same value (after stripping casts)
/// is equivalent to that common value.
/// Example: select %cond, %p, %p and %p
static bool checkTrivialSelect(const Value *S1, const Value *S2) {
  if (auto *SI = dyn_cast<SelectInst>(S1)) {
    if (stripNoopCasts(SI->getTrueValue()) == S2 &&
        stripNoopCasts(SI->getFalseValue()) == S2)
      return true;
    if (const auto *Cond = dyn_cast<ConstantInt>(SI->getCondition())) {
      if (Cond->isOne() && stripNoopCasts(SI->getTrueValue()) == S2)
        return true;
      if (Cond->isZero() && stripNoopCasts(SI->getFalseValue()) == S2)
        return true;
    }
  }

  if (auto *SI = dyn_cast<SelectInst>(S2)) {
    if (stripNoopCasts(SI->getTrueValue()) == S1 &&
        stripNoopCasts(SI->getFalseValue()) == S1)
      return true;
    if (const auto *Cond = dyn_cast<ConstantInt>(SI->getCondition())) {
      if (Cond->isOne() && stripNoopCasts(SI->getTrueValue()) == S1)
        return true;
      if (Cond->isZero() && stripNoopCasts(SI->getFalseValue()) == S1)
        return true;
    }
  }

  return false;
}

/// Rule 10: Same Allocation Site
/// Two pointers derived from the same allocation call (malloc, new, etc.)
/// must alias. Sound: each allocation returns a unique address.
/// Example: %p = call malloc(...); %q = bitcast %p; then %p ≡ %q
static bool checkSameAllocation(const Value *S1, const Value *S2) {
  return checkSameAllocationSite(S1, S2);
}

/// Rule 11: Enhanced Round-Trip Cast
/// A pointer converted to integer and back with no-op arithmetic is unchanged.
/// Sound: no-op arithmetic (add 0, mul 1, etc.) preserves the value.
/// Example: inttoptr(ptrtoint(%p) + 0) and %p
static bool checkEnhancedRoundTrip(const Value *S1, const Value *S2) {
  return isEnhancedRoundTrip(S1, S2);
}

static bool sameIntegerOpPoisonFlags(const Operator *A, const Operator *B) {
  if (!A || !B || A->getOpcode() != B->getOpcode())
    return false;

  const auto *OA = dyn_cast<OverflowingBinaryOperator>(A);
  const auto *OB = dyn_cast<OverflowingBinaryOperator>(B);
  if (static_cast<bool>(OA) != static_cast<bool>(OB))
    return false;
  if (OA && OB) {
    if (OA->hasNoSignedWrap() != OB->hasNoSignedWrap())
      return false;
    if (OA->hasNoUnsignedWrap() != OB->hasNoUnsignedWrap())
      return false;
  }

  const auto *EA = dyn_cast<PossiblyExactOperator>(A);
  const auto *EB = dyn_cast<PossiblyExactOperator>(B);
  if (static_cast<bool>(EA) != static_cast<bool>(EB))
    return false;
  if (EA && EB && EA->isExact() != EB->isExact())
    return false;

  return true;
}

static bool equivalentIntegerExpr(const Value *A, const Value *B,
                                  unsigned Depth = 0) {
  if (A == B)
    return true;
  if (!A || !B || Depth > 10)
    return false;
  if (!A->getType()->isIntegerTy() || !B->getType()->isIntegerTy())
    return false;

  if (const auto *CA = dyn_cast<ConstantInt>(A))
    if (const auto *CB = dyn_cast<ConstantInt>(B))
      return CA->getValue() == CB->getValue();

  const auto *OpA = dyn_cast<Operator>(A);
  const auto *OpB = dyn_cast<Operator>(B);
  if (!OpA || !OpB)
    return false;

  const unsigned Opc = OpA->getOpcode();
  if (Opc != OpB->getOpcode() || OpA->getNumOperands() != OpB->getNumOperands())
    return false;
  if (OpA->getType() != OpB->getType())
    return false;

  switch (Opc) {
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::Trunc:
  case Instruction::Freeze:
    return equivalentIntegerExpr(OpA->getOperand(0), OpB->getOperand(0),
                                 Depth + 1);
  case Instruction::Add:
  case Instruction::Sub:
  case Instruction::Mul:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
  case Instruction::Shl:
  case Instruction::AShr:
  case Instruction::LShr: {
    if (!sameIntegerOpPoisonFlags(OpA, OpB))
      return false;

    const bool Direct = equivalentIntegerExpr(OpA->getOperand(0),
                                              OpB->getOperand(0), Depth + 1) &&
                        equivalentIntegerExpr(OpA->getOperand(1),
                                              OpB->getOperand(1), Depth + 1);
    if (Direct)
      return true;

    if (!Instruction::isCommutative(Opc))
      return false;
    return equivalentIntegerExpr(OpA->getOperand(0), OpB->getOperand(1),
                                 Depth + 1) &&
           equivalentIntegerExpr(OpA->getOperand(1), OpB->getOperand(0),
                                 Depth + 1);
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Main atomic must-alias checker
//===----------------------------------------------------------------------===//

/// Checks if two pointers must alias using only local, syntactic rules.
/// This is the "atomic" test used to seed the union-find propagation.
///
/// Returns true if the pointers are guaranteed to alias, false otherwise.
/// Never produces false positives (sound under-approximation).
static bool atomicMustAlias(const DataLayout &DL, const Value *A,
                            const Value *B) {
  // Normalize by stripping no-op casts first
  const Value *S1 = stripNoopCasts(A);
  const Value *S2 = stripNoopCasts(B);

  // Apply each rule in sequence (order doesn't matter for correctness,
  // but checking cheaper rules first may improve performance)
  if (checkIdentity(S1, S2))
    return true;
  if (checkCastEquivalence(S1, S2))
    return true;
  if (checkConstOffsetGEP(DL, S1, S2))
    return true;
  if (checkZeroGEP(S1, S2))
    return true;
  if (checkSameGEPOperands(S1, S2))
    return true;
  if (checkRoundTripCast(S1, S2))
    return true;
  if (checkSameUnderlyingObject(S1, S2))
    return true;
  if (checkConstantNull(S1, S2))
    return true;
  if (checkTrivialPHI(S1, S2))
    return true;
  if (checkTrivialSelect(S1, S2))
    return true;
  // New rules for extended analysis
  if (checkSameAllocation(S1, S2))
    return true;
  if (checkEnhancedRoundTrip(S1, S2))
    return true;
  // No rule matched - cannot prove they must alias
  return false;
}

//===----------------------------------------------------------------------===//
//             2.  Union–find helpers
//===----------------------------------------------------------------------===//

/// Get or create a unique ID for a value
///
/// Each value in the function gets a unique integer ID that serves as an
/// index into the union-find data structures. IDs are allocated lazily as
/// values are encountered during analysis.
///
/// @param V The value to get an ID for
/// @return The unique ID for V (existing or newly allocated)
///
/// Time complexity: O(1) amortized (hash map lookup/insert + vector append)
EquivDB::IdTy EquivDB::id(const Value *V) {
  // Safety check: ensure V is not null.
  // We use a dedicated sentinel slot (index 0) that is pre-allocated in the
  // constructor so that id(nullptr) never collides with any real value's ID.
  // Real value IDs start at 1.
  if (!V)
    return 0; // Sentinel slot — never unified with real values

  // Check if this value already has an ID
  auto It = Val2Id.find(V);
  if (It != Val2Id.end())
    return It->second;

  // Allocate a new ID (the current size of Nodes/Id2Val).
  // Because the sentinel occupies slot 0, the first real value gets ID 1.
  IdTy New = Nodes.size();

  // Initialize union-find node: parent is self (root), rank is 0
  Nodes.push_back({New, 0});

  // Maintain bidirectional mapping: ID → Value and Value → ID
  Id2Val.push_back(V);
  Val2Id[V] = New;

  // Initialize empty watch list for this equivalence class
  Watches.emplace_back();

  return New;
}

/// Find the root of the equivalence class containing X (with path compression)
///
/// This implements the standard union-find find operation with path compression
/// optimization. Path compression flattens the tree by making all nodes point
/// directly to the root during traversal, improving future queries.
///
/// @param X The ID to find the root for
/// @return The root ID of the equivalence class containing X
///
/// Time complexity: O(α(N)) amortized where α is the inverse Ackermann function
/// (effectively constant for all practical purposes)
EquivDB::IdTy EquivDB::find(IdTy X) {
  // Safety check: ensure X is within bounds
  if (X >= Nodes.size())
    return X; // Invalid ID, return as-is

  // Base case: X is the root (parent points to itself)
  if (Nodes[X].Parent == X)
    return X;

  // Safety check: ensure parent is within bounds
  if (Nodes[X].Parent >= Nodes.size()) {
    // Invalid parent, fix it to point to self
    Nodes[X].Parent = X;
    return X;
  }

  // Recursive case: find root and compress path
  // Path compression: set parent directly to root (flattening the tree)
  return Nodes[X].Parent = find(Nodes[X].Parent);
}

/// Unite two equivalence classes (union-by-rank with watch list merging)
///
/// This implements the union operation of union-find with union-by-rank
/// optimization. Union-by-rank keeps trees shallow by always attaching the
/// shorter tree under the taller one. This preserves the O(α(N)) amortized
/// time complexity.
///
/// Additionally, when two classes merge, their watch lists must be merged
/// so that instructions watching either class will be revisited when the
/// unified class changes.
///
/// @param A First equivalence class ID (will be normalized to root)
/// @param B Second equivalence class ID (will be normalized to root)
///
/// Time complexity: O(α(N)) amortized + O(W) where W is the size of the
/// smaller watch list (dominated by watch list merge)
void EquivDB::unite(IdTy A, IdTy B) {
  // Normalize to roots first
  A = find(A);
  B = find(B);

  // Already in the same class - nothing to do
  if (A == B)
    return;

  // Union-by-rank: attach smaller tree under larger tree
  // This keeps trees balanced and preserves O(α(N)) complexity
  if (Nodes[A].Rank < Nodes[B].Rank)
    std::swap(A, B);

  // Attach B's tree under A's tree
  Nodes[B].Parent = A;

  // If ranks were equal, the new tree is one level taller
  if (Nodes[A].Rank == Nodes[B].Rank)
    ++Nodes[A].Rank;

  // Merge watch lists: all instructions watching class B should now
  // watch the unified class A. This ensures that when class A changes,
  // we revisit instructions that originally depended on B.
  auto &Dst = Watches[A].Users;       // Destination (class A's watch list)
  auto &Src = Watches[B].Users;       // Source (class B's watch list)
  Dst.append(Src.begin(), Src.end()); // Append B's instructions to A's list
  Src.clear();                        // Clear B's list (it's now merged into A)
}

//===----------------------------------------------------------------------===//
//                    3. Build (seed + propagate)
//===----------------------------------------------------------------------===//

/// Check if all pointer operands of an instruction belong to the same
/// equivalence class
///
/// This helper is used by semantic rules to detect when an instruction's
/// pointer operands have unified into a single class. When this happens,
/// certain patterns (like PHI nodes or Select instructions) can be simplified.
///
/// @param I The instruction to check
/// @param DB The equivalence database to query
/// @return A representative value from the unified class if all pointer
/// operands
///         are in the same class, nullptr otherwise
///
/// Examples:
///   - PHI [%p, bb1], [%p, bb2]: all operands = %p → returns %p
///   - PHI [%p, bb1], [%q, bb2]: if %p ≡ %q → returns %p (or %q)
///   - PHI [%p, bb1], [%q, bb2]: if %p ≠ %q → returns nullptr
///   - Select %c, %p, %q: if %p ≡ %q → returns %p
static const Value *uniquePtrOperandClass(const Instruction *I,
                                          const EquivDB &DB) {
  const Value *Rep = nullptr;

  // Iterate over all operands, checking only pointer-typed ones
  for (const Value *Op : I->operands())
    if (Op->getType()->isPointerTy()) {
      if (!Rep) {
        // First pointer operand becomes the representative
        Rep = Op;
      } else if (!DB.mustAlias(Rep, Op)) {
        // Found a pointer operand in a different class - not unified
        return nullptr;
      }
      // If we get here, Op is in the same class as Rep
    }

  // All pointer operands (if any) are in the same class
  // Return the representative (or nullptr if no pointer operands)
  return Rep;
}

size_t EquivDB::SlotIdHash::operator()(const SlotId &Slot) const {
  return static_cast<size_t>(hash_combine(Slot.Object, Slot.ByteOffset));
}

size_t EquivDB::TermKeyHash::operator()(const TermKey &Key) const {
  hash_code H = hash_value(static_cast<unsigned>(Key.Kind));
  for (uint64_t Component : Key.Components)
    H = hash_combine(H, Component);
  return static_cast<size_t>(H);
}

/// Semantic Rule S1: Closed PHI Node
///
/// A PHI node is "closed" when all of its incoming pointer values belong
/// to the same equivalence class. When this happens, the PHI node itself
/// must equal that common class.
///
/// This rule is inductive: as more values unify during propagation,
/// previously non-closed PHIs may become closed and trigger further
/// unification.
///
/// @param I The instruction to check (should be a PHINode)
/// @param DB The equivalence database
/// @param Rep Output parameter: set to the representative value if rule fires
/// @return true if the PHI is closed and the rule applies, false otherwise
///
/// Example:
///   %phi = phi i8* [%p, %bb1], [%q, %bb2]
///   If %p ≡ %q (after propagation), then %phi ≡ %p (rule fires)
///
/// This is sound because if all paths through the PHI produce the same
/// pointer (in the equivalence sense), the PHI itself must produce that
/// pointer.
static bool ruleClosedPHI(const Instruction *I, const EquivDB &DB,
                          const Value *&Rep) {
  auto *PN = dyn_cast<PHINode>(I);
  if (!PN)
    return false;

  // Check if all incoming values are in the same class
  if (const Value *Common = uniquePtrOperandClass(PN, DB)) {
    Rep = Common; // PHI equals the common representative
    return true;  // Rule fired successfully
  }
  return false; // PHI not closed - operands still in different classes
}

/// Semantic Rule S2: Closed Select Instruction
///
/// A Select instruction is "closed" when both its true and false branch
/// values (if pointers) belong to the same equivalence class. When this
/// happens, the Select itself must equal that common class, regardless of
/// the condition.
///
/// This rule is inductive: as more values unify, previously non-closed
/// Selects may become closed.
///
/// @param I The instruction to check (should be a SelectInst)
/// @param DB The equivalence database
/// @param Rep Output parameter: set to the representative value if rule fires
/// @return true if the Select is closed and the rule applies, false otherwise
///
/// Example:
///   %sel = select i1 %cond, i8* %p, i8* %q
///   If %p ≡ %q (after propagation), then %sel ≡ %p (rule fires)
///
/// This is sound because if both branches produce the same pointer
/// (in the equivalence sense), the Select must produce that pointer regardless
/// of which branch is taken.
static bool ruleClosedSelect(const Instruction *I, const EquivDB &DB,
                             const Value *&Rep) {
  auto *SI = dyn_cast<SelectInst>(I);
  if (!SI)
    return false;

  // Check if both true and false values are in the same class
  if (const Value *Common = uniquePtrOperandClass(SI, DB)) {
    Rep = Common; // Select equals the common representative
    return true;  // Rule fired successfully
  }
  return false; // Select not closed - branches still in different classes
}

/// Constructor: Build the equivalence database for a function
///
/// This is a three-phase process:
/// 1. Seed: Apply atomic (syntactic) rules to find initial must-alias pairs
/// 2. Propagate: Use semantic (inductive) rules to discover additional
/// equivalences
/// 3. Refine: Apply external analyses (MemorySSA, DominatorTree) for more
/// precision
///
/// The database is built eagerly on construction. Queries after construction
/// are O(α(N)) in the number of values.
///
/// @param Func The LLVM function to analyze
/// @param MemSSA Optional MemorySSA for store-load forwarding (sound)
/// @param DomTree Optional DominatorTree for single-store alloca forwarding
///
/// Time complexity: O(N·M·α(N)) where N = number of values, M = number of
/// instructions. In practice, the α(N) factor is effectively constant.
EquivDB::EquivDB(Function &Func, MemorySSA *MemSSA, DominatorTree *DomTree)
    : DL(Func.getParent()->getDataLayout()), F(Func), MSSA(MemSSA),
      DT(DomTree) {

  // Pre-allocate sentinel slot 0 for id(nullptr).
  // This ensures that the first real value gets ID 1, not 0, so that
  // id(nullptr)==0 never collides with any real value's ID.
  // The sentinel node is a self-rooted union-find node that is never
  // unified with anything else.
  Nodes.push_back({0u, 0u}); // parent=self, rank=0
  Id2Val.push_back(nullptr); // slot 0 → nullptr
  Watches.emplace_back();    // empty watch list for sentinel

  // Safety check: ensure function has a parent module
  if (!Func.getParent()) {
    // Function without a parent module - cannot build equivalence database
    // Return empty database (all queries will return false)
    // Note: DL reference is invalid in this case, but we won't use it since
    // we return early. This is safe because we check Func.empty() before using
    // DL.
    return;
  }

  // Safety check: ensure function has basic blocks (not a declaration)
  if (Func.empty()) {
    // Function declaration without body - cannot build equivalence database
    // Return empty database (all queries will return false)
    return;
  }

  // Worklist stores pairs of values that must alias (to be unified)
  std::vector<std::pair<const Value *, const Value *>> WorkList;
  buildGEPIndex();

  // Phase 1: Seed the worklist with atomic (syntactic) must-alias pairs
  // These are patterns we can detect locally without knowing the full
  // equivalence structure (e.g., bitcasts, zero GEPs, etc.)
  seedAtomicEqualities(WorkList);

  // Phase 1a: Seed from direct calls that return a pointer argument
  // Sound: callee "return arg_i" ⇒ call result must-alias arg_i at call site
  seedReturnValueForwarding(WorkList);

  // Phase 1b: Seed from singleton slots when DominatorTree available.
  // Sound: one store to a singleton slot dominating all loads from that slot
  // ⇒ load equals stored value.
  if (DT) {
    seedSingleStoreAlloca(WorkList);
  }

  // Phase 2: Propagate equivalences using semantic (inductive) rules
  // As we unify classes, new patterns may emerge (e.g., closed PHIs),
  // which add more pairs to the worklist until saturation.
  propagate(WorkList);

  // Phase 3: Iterative MemorySSA forwarding (if available)
  // Memory forwarding benefits from discovered equivalence classes.
  // Re-seed from memory clobbers and re-propagate to a local fixpoint.
  if (MSSA) {
    while (true) {
      const size_t Before = SeenPairKeys.size();
      seedStoreLoadForwarding(WorkList);
      if (SeenPairKeys.size() == Before)
        break;
      propagate(WorkList);
    }
  }
}

const Value *EquivDB::valueForRoot(IdTy Root) const {
  Root = const_cast<EquivDB *>(this)->find(Root);
  if (Root >= Id2Val.size())
    return nullptr;
  return Id2Val[Root];
}

const Value *EquivDB::resolveNormalizedValue(const Value *V) const {
  if (!V || !V->getType()->isPointerTy())
    return V;

  V = stripNoopCasts(V);

  if (const auto *GEP = dyn_cast<GEPOperator>(V))
    if (GEP->hasAllZeroIndices())
      return stripNoopCasts(GEP->getPointerOperand());

  const auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return V;

  const Value *Rep = nullptr;
  if (ruleClosedPHI(I, *this, Rep) || ruleClosedSelect(I, *this, Rep)) {
    IdTy Root = const_cast<EquivDB *>(this)->find(
        const_cast<EquivDB *>(this)->id(Rep));
    if (const Value *Normalized = valueForRoot(Root))
      return Normalized;
    return Rep;
  }

  return V;
}

bool EquivDB::buildNormalizedTermKey(const Value *V, TermKey &Key) const {
  Key = TermKey{};

  auto *GEP = dyn_cast<GEPOperator>(V);
  if (!GEP)
    return false;

  APInt Offset(DL.getPointerSizeInBits(GEP->getPointerAddressSpace()), 0);
  const Value *Base = V->stripAndAccumulateInBoundsConstantOffsets(DL, Offset);
  if (Base != V) {
    Base = resolveNormalizedValue(Base);
    IdTy BaseRoot = const_cast<EquivDB *>(this)->find(
        const_cast<EquivDB *>(this)->id(Base));
    Key.Kind = TermKind::GEPConst;
    Key.Components.push_back(static_cast<uint64_t>(BaseRoot));
    Key.Components.push_back(encodeSignedInt64(Offset.getSExtValue()));
    Key.Components.push_back(
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
            GEP->getSourceElementType())));
    return true;
  }

  const Value *BasePtr = resolveNormalizedValue(GEP->getPointerOperand());
  IdTy BaseRoot = const_cast<EquivDB *>(this)->find(
      const_cast<EquivDB *>(this)->id(BasePtr));

  Key.Kind = TermKind::GEPIndex;
  Key.Components.push_back(static_cast<uint64_t>(BaseRoot));
  Key.Components.push_back(
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
          GEP->getSourceElementType())));
  Key.Components.push_back(static_cast<uint64_t>(GEP->getNumOperands()));

  for (unsigned I = 1, E = GEP->getNumOperands(); I < E; ++I) {
    const Value *Idx = GEP->getOperand(I);
    if (Idx->getType()->isIntegerTy()) {
      Idx = stripNoopArithmetic(Idx);
      if (const auto *CI = dyn_cast<ConstantInt>(Idx)) {
        Key.Components.push_back(1);
        Key.Components.push_back(CI->getValue().getSExtValue());
        continue;
      }
      if (const auto *Op = dyn_cast<Operator>(Idx)) {
        Key.Components.push_back(2);
        Key.Components.push_back(Op->getOpcode());
        Key.Components.push_back(
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(Op->getType())));
        continue;
      }
    }

    Key.Components.push_back(3);
    Key.Components.push_back(
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(Idx->getType())));
    Key.Components.push_back(
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(Idx)));
  }

  return true;
}

bool EquivDB::termsCongruent(const Value *A, const Value *B,
                             const TermKey &Key) const {
  auto *GEPA = dyn_cast<GEPOperator>(A);
  auto *GEPB = dyn_cast<GEPOperator>(B);
  if (!GEPA || !GEPB)
    return false;

  switch (Key.Kind) {
  case TermKind::GEPConst: {
    APInt OffA(DL.getPointerSizeInBits(GEPA->getPointerAddressSpace()), 0);
    APInt OffB(DL.getPointerSizeInBits(GEPB->getPointerAddressSpace()), 0);
    const Value *BaseA = A->stripAndAccumulateInBoundsConstantOffsets(DL, OffA);
    const Value *BaseB = B->stripAndAccumulateInBoundsConstantOffsets(DL, OffB);
    if (OffA != OffB)
      return false;
    BaseA = resolveNormalizedValue(BaseA);
    BaseB = resolveNormalizedValue(BaseB);
    const_cast<EquivDB *>(this)->id(BaseA);
    const_cast<EquivDB *>(this)->id(BaseB);
    return BaseA && BaseB && mustAlias(BaseA, BaseB) &&
           GEPA->getSourceElementType() == GEPB->getSourceElementType();
  }
  case TermKind::GEPIndex:
    if (GEPA->getSourceElementType() != GEPB->getSourceElementType())
      return false;
    if (GEPA->getNumOperands() != GEPB->getNumOperands())
      return false;

    if (!mustAlias(resolveNormalizedValue(GEPA->getPointerOperand()),
                   resolveNormalizedValue(GEPB->getPointerOperand())))
      return false;

    for (unsigned I = 1, E = GEPA->getNumOperands(); I < E; ++I)
      if (!equivalentGEPIndexOperand(GEPA->getOperand(I), GEPB->getOperand(I)))
        return false;
    return true;
  case TermKind::Invalid:
    return false;
  }

  return false;
}

void EquivDB::propagateNormalizedTerm(
    Instruction *I,
    std::vector<std::pair<const Value *, const Value *>> &WL) {
  if (!I || !I->getType()->isPointerTy())
    return;

  if (const Value *Rep = resolveNormalizedValue(I)) {
    if (Rep != I)
      enqueuePair(WL, I, Rep);
  }

  TermKey Key;
  if (!buildNormalizedTermKey(I, Key))
    return;

  auto &Bucket = TermBuckets[Key];
  for (const Value *Other : Bucket) {
    if (Other == I)
      return;
    if (termsCongruent(I, Other, Key)) {
      enqueuePair(WL, I, Other);
      return;
    }
  }

  Bucket.push_back(I);
}

bool EquivDB::tryGetSingletonSlot(const Value *Ptr, SlotId &Out) const {
  if (!Ptr || !Ptr->getType()->isPointerTy())
    return false;

  const unsigned AddrSpace = Ptr->getType()->getPointerAddressSpace();
  APInt Offset(DL.getPointerSizeInBits(AddrSpace), 0);
  const Value *Base = Ptr->stripAndAccumulateInBoundsConstantOffsets(DL, Offset);
  const Value *Obj = getUnderlyingObject(Ptr);
  if (!Obj || Base != Obj)
    return false;
  if (!isa<AllocaInst>(Obj) && !isa<GlobalVariable>(Obj) &&
      !isAllocationCall(Obj))
    return false;

  Out.Object = Obj;
  Out.ByteOffset = Offset.getSExtValue();
  return true;
}

bool EquivDB::sameSingletonSlot(const Value *P, const Value *Q) const {
  SlotId SP, SQ;
  return tryGetSingletonSlot(P, SP) && tryGetSingletonSlot(Q, SQ) && SP == SQ;
}

uint64_t EquivDB::internSlot(const SlotId &Slot) {
  auto It = SlotNumbers.find(Slot);
  if (It != SlotNumbers.end())
    return It->second;

  const uint64_t Next = SlotNumbers.size() + 1;
  SlotNumbers.emplace(Slot, Next);
  return Next;
}

/// Phase 1: Seed the worklist with atomic (syntactic) must-alias pairs
///
/// This function scans all instructions in the function and applies atomic
/// must-alias rules. These rules are:
/// - Local: only look at the instruction and its immediate operands
/// - Syntactic: detect patterns in the IR structure, not semantic relationships
/// - Sound: never produce false positives (conservative under-approximation)
///
/// Additionally, this function registers "watches" on pointer-producing
/// instructions. When the equivalence classes of an instruction's operands
/// merge, we revisit that instruction to check if semantic rules can now fire.
///
/// @param WL Output worklist to populate with must-alias pairs
///
/// Time complexity: O(N·M) where N = number of instructions, M = average
/// number of operands per instruction (typically small, bounded by ~10)
void EquivDB::seedAtomicEqualities(
    std::vector<std::pair<const Value *, const Value *>> &WL) {
  // Helper lambda to add a must-alias pair to the worklist
  // Only adds pairs if both values are pointers and they must-alias
  auto push = [&](const Value *A, const Value *B) {
    if (!A->getType()->isPointerTy() || !B->getType()->isPointerTy())
      return;
    if (atomicMustAlias(DL, A, B))
      enqueuePair(WL, A, B);
  };

  // Scan all instructions in all basic blocks
  for (BasicBlock &BB : F)
    for (Instruction &I : BB) {
      // Pattern 1: Check result ↔ operand pairs
      // Examples:
      //   - %p = bitcast %q: check if %p ≡ %q
      //   - %r = gep %p, 0: check if %r ≡ %p (if zero GEP)
      for (Value *Op : I.operands())
        push(&I, Op);

      // Pattern 2: Check operand ↔ operand pairs
      // This is needed for PHI and Select instructions where we want to
      // detect when multiple operands must-alias (e.g., PHI with same value
      // on all edges, or Select with same value in both branches)
      //
      // Examples:
      //   - PHI [%p, %bb1], [%p, %bb2]: check if operands ≡ (trivial PHI rule)
      //   - Select %c, %p, %p: check if operands ≡ (trivial Select rule)
      for (unsigned i = 0, e = I.getNumOperands(); i < e; ++i)
        for (unsigned j = i + 1; j < e; ++j)
          push(I.getOperand(i), I.getOperand(j));

      // Register pointer-producing instructions for semantic rule checking
      // When the equivalence classes of an instruction's pointer operands
      // merge, we need to revisit the instruction to see if semantic rules
      // (like closed PHI or closed Select) can now fire.
      //
      // Example:
      //   %phi = phi [%p, %bb1], [%q, %bb2]
      //   We watch the classes of %p and %q. If they later unify during
      //   propagation, we revisit %phi and the closed PHI rule may fire.
      if (!I.getType()->isPointerTy())
        continue;
      for (Value *Op : I.operands())
        if (Op->getType()->isPointerTy())
          registerWatch(Op, &I);
      propagateNormalizedTerm(&I, WL);
    }
}

/// Phase 1a: Seed from calls that return a pointer argument
///
/// If the callee always returns one pointer argument (possibly after no-op
/// casts), the call result must-alias that argument at the call site.
///
/// This implementation is intentionally conservative and sound:
/// - Handles all CallBase instructions (call/invoke/callbr)
/// - Uses `returned` parameter attributes when present
/// - For direct callees with bodies, accepts multiple returns only when all
///   return the same pointer argument
void EquivDB::seedReturnValueForwarding(
    std::vector<std::pair<const Value *, const Value *>> &WL) {
  // SummaryCache is shared across all EquivDB instances (one per function
  // being analyzed).  Under parallel compilation (e.g. ThinLTO or a
  // multi-threaded pass manager) multiple threads may call this method
  // concurrently for different functions, each of which may call different
  // callees.  Protect the cache with a mutex so concurrent reads/writes are
  // safe.  The lock is only held while looking up / inserting into the cache,
  // not while running summarizeReturnBehavior(), so contention is minimal.
  static std::unordered_map<const Function *, ReturnSummary> SummaryCache;
  static std::mutex SummaryCacheMutex;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB || !CB->getType()->isPointerTy())
        continue;

      auto pushReturnedArg = [&](unsigned ArgNo) {
        if (ArgNo >= CB->arg_size())
          return;
        const Value *CallArg = CB->getArgOperand(ArgNo);
        if (!CallArg->getType()->isPointerTy())
          return;
        enqueuePair(WL, CB, CallArg);
      };

      SmallVector<unsigned, 4> ReturnedAttrArgs;
      // Attribute-based forwarding works even for declarations.
      for (unsigned ArgNo = 0, E = CB->arg_size(); ArgNo < E; ++ArgNo)
        if (CB->paramHasAttr(ArgNo, Attribute::Returned)) {
          ReturnedAttrArgs.push_back(ArgNo);
          pushReturnedArg(ArgNo);
        }
      // If multiple args are marked returned, they must alias each other.
      for (unsigned i = 0; i < ReturnedAttrArgs.size(); ++i)
        for (unsigned j = i + 1; j < ReturnedAttrArgs.size(); ++j) {
          const Value *A = CB->getArgOperand(ReturnedAttrArgs[i]);
          const Value *B = CB->getArgOperand(ReturnedAttrArgs[j]);
          if (A->getType()->isPointerTy() && B->getType()->isPointerTy())
            enqueuePair(WL, A, B);
        }

      Function *Callee = CB->getCalledFunction();
      if (!Callee)
        continue;

      // Look up the cache under the lock; compute the summary outside the
      // lock if it is missing, then re-acquire to insert.
      // Using a lambda avoids goto-across-initialization issues.
      ReturnSummary Summary = [&]() -> ReturnSummary {
        {
          std::lock_guard<std::mutex> Guard(SummaryCacheMutex);
          auto It = SummaryCache.find(Callee);
          if (It != SummaryCache.end())
            return It->second; // cache hit — no computation needed
        }
        // Compute outside the lock (read-only IR traversal, no shared state).
        ReturnSummary Computed = summarizeReturnBehavior(Callee);
        {
          std::lock_guard<std::mutex> Guard(SummaryCacheMutex);
          // Another thread may have inserted while we were computing; prefer
          // the existing entry (both are equivalent for the same callee).
          SummaryCache.emplace(Callee, Computed);
        }
        return Computed;
      }();

      switch (Summary.K) {
      case ReturnSummary::Kind::Arg:
        pushReturnedArg(static_cast<unsigned>(Summary.ArgNo));
        break;
      case ReturnSummary::Kind::Null:
      case ReturnSummary::Kind::Global:
        if (Summary.Fixed && Summary.Fixed->getType() == CB->getType())
          enqueuePair(WL, CB, Summary.Fixed);
        break;
      case ReturnSummary::Kind::Unknown:
        break;
      }
    }
  }
}

/// Phase 1c: Seed from singleton slots when DominatorTree available
///
/// For each load from a singleton slot, if there is exactly one dominating
/// store (pointer-typed value) to that slot, then the load must-alias the
/// stored value. This handles both the classic single-store case and per-load
/// unique reaching store patterns for allocas/globals at constant offsets.
void EquivDB::seedSingleStoreAlloca(
    std::vector<std::pair<const Value *, const Value *>> &WL) {
  if (!DT)
    return;

  using SlotStores = SmallVector<StoreInst *, 2>;
  using SlotLoads = SmallVector<LoadInst *, 4>;
  llvm::DenseMap<uint64_t, SlotStores> SlotToStores;
  llvm::DenseMap<uint64_t, SlotLoads> SlotToLoads;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        SlotId Slot;
        if (tryGetSingletonSlot(SI->getPointerOperand(), Slot))
          SlotToStores[internSlot(Slot)].push_back(SI);
      } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (!LI->getType()->isPointerTy())
          continue;
        SlotId Slot;
        if (tryGetSingletonSlot(LI->getPointerOperand(), Slot))
          SlotToLoads[internSlot(Slot)].push_back(LI);
      }
    }
  }

  // For each singleton slot that has both stores and loads, check per-load
  // uniqueness.
  for (auto &KV : SlotToLoads) {
    const uint64_t SlotNo = KV.first;
    auto SIt = SlotToStores.find(SlotNo);
    if (SIt == SlotToStores.end())
      continue;

    const SlotStores &Stores = SIt->second;
    const SlotLoads &Loads = KV.second;

    for (LoadInst *LI : Loads) {
      StoreInst *UniqueStore = nullptr;
      bool Ambiguous = false;

      for (StoreInst *SI : Stores) {
        if (!dominatesInst(SI, LI))
          continue;
        if (!sameSingletonSlot(SI->getPointerOperand(), LI->getPointerOperand()))
          continue;

        if (!UniqueStore) {
          UniqueStore = SI;
        } else if (UniqueStore != SI) {
          Ambiguous = true;
          break;
        }
      }

      if (!UniqueStore || Ambiguous)
        continue;

      const Value *StoredVal = UniqueStore->getValueOperand();
      if (!StoredVal->getType()->isPointerTy())
        continue;

      enqueuePair(WL, LI, StoredVal);
    }
  }
}

/// Register an instruction to watch an operand's equivalence class
///
/// When an instruction I depends on a pointer operand Op, we register I
/// to "watch" the equivalence class containing Op. When that class merges
/// with another class, we revisit I to check if semantic rules can fire.
///
/// Example:
///   %phi = phi [%p, %bb1], [%q, %bb2]
///   registerWatch(%p, %phi) → %phi watches class(%p)
///   registerWatch(%q, %phi) → %phi watches class(%q)
///   Later, if class(%p) merges with class(%q), we revisit %phi and
///   the closed PHI rule may fire.
///
/// @param Op The pointer operand that the instruction depends on
/// @param I The instruction to register (should produce a pointer)
///
/// Time complexity: O(α(N)) amortized (find operation)
void EquivDB::registerWatch(const Value *Op, Instruction *I) {
  // Get the root of the equivalence class containing Op
  IdTy C = find(id(Op));

  // Add I to the watch list for this class
  auto &Vec = Watches[C].Users;
  Vec.push_back(I);

  // Note: The same instruction may be added to multiple watch lists
  // (e.g., a PHI watching multiple incoming values). This is fine - we
  // check semantic rules only when needed, and duplicate checks are harmless.
}

/// Phase 2: Propagate equivalences using normalized pointer terms
///
/// This function processes the worklist until saturation. For each pair
/// (A, B) that must-alias:
/// 1. Unify their equivalence classes
/// 2. Revisit all instructions watching the merged class
/// 3. Check if semantic rules can fire (e.g., closed PHI, closed Select)
/// 4. Add new pairs to the worklist if rules fire
///
/// The process continues until no new equivalences can be discovered.
/// This is guaranteed to terminate because:
/// - There are finitely many values
/// - Each unification reduces the number of equivalence classes
/// - Semantic rules can fire at most once per instruction (after unification)
///
/// @param WL The worklist of must-alias pairs (modified in-place)
///
/// Time complexity: O(N·R·α(N)) where N = number of values, R = number of
/// semantic rules. In practice, most instructions are revisited only once or
/// twice.
void EquivDB::propagate(
    std::vector<std::pair<const Value *, const Value *>> &WL) {

  // Process worklist until saturation (no new pairs added)
  while (!WL.empty()) {
    // Pop a must-alias pair from the worklist
    const Value *A = WL.back().first;
    const Value *B = WL.back().second;
    WL.pop_back();

    // Normalize to roots (find canonical representatives)
    IdTy CA = find(id(A));
    IdTy CB = find(id(B));

    // Skip if already in the same class (duplicate or redundant)
    if (CA == CB)
      continue;

    // Unify the two classes
    unite(CA, CB);

    // Get the new root after unification (could be CA or CB depending on ranks)
    IdTy NewRoot = find(CA);

    // Lambda to revisit instructions watching a class
    // When classes merge, instructions watching the merged class may now
    // satisfy semantic rules (e.g., all PHI operands are now in the same class)
    auto Revisit = [&](IdTy Cls) {
      auto &List = Watches[Cls].Users;
      SmallVector<Instruction *, 8> Snapshot(List.begin(), List.end());

      // Check each instruction watching this class
      for (Instruction *I : Snapshot) {
        propagateNormalizedTerm(I, WL);
      }
    };

    // Revisit instructions watching the merged class
    // The merged watch list is now at NewRoot (after unite() merged Watches[B]
    // into Watches[A])
    Revisit(NewRoot);
  }

  // Worklist exhausted - no more equivalences can be discovered
  // The database is now complete and ready for queries
}

//===----------------------------------------------------------------------===//
//                               Query Interface
//===----------------------------------------------------------------------===//

/// Query if two values must alias (are in the same equivalence class)
///
/// This is the main query interface for the equivalence database. After
/// construction, queries are very fast (effectively constant time).
///
/// @param A First value to compare
/// @param B Second value to compare
/// @return true if A and B are guaranteed to alias (same equivalence class),
///         false if unknown (they may or may not alias)
///
/// Behavior:
/// - Returns false if either value was never encountered during construction
///   (e.g., it's not a pointer, or not in this function)
/// - Returns true if A and B are in the same equivalence class (must alias)
/// - Returns false if A and B are in different classes (unknown - may or may
/// not alias)
///
/// Time complexity: O(α(N)) amortized ≈ O(1) in practice
///
/// Soundness: This is a sound under-approximation:
/// - If returns true → A and B are guaranteed to alias (no false positives)
/// - If returns false → unknown (may be false negative - they might still
/// alias)
bool EquivDB::mustAlias(const Value *A, const Value *B) const {
  // Look up IDs for both values
  auto It1 = Val2Id.find(A);
  auto It2 = Val2Id.find(B);

  // If either value wasn't encountered during construction, it's not a pointer
  // or not in this function - cannot prove must-alias
  if (It1 == Val2Id.end() || It2 == Val2Id.end())
    return false;

  // Check if both values are in the same equivalence class (same root)
  // Note: const_cast needed because find() uses path compression (mutates tree)
  // This is safe - path compression is an optimization that preserves
  // correctness
  return const_cast<EquivDB *>(this)->find(It1->second) ==
         const_cast<EquivDB *>(this)->find(It2->second);
}

//===----------------------------------------------------------------------===//
//                    Extension: Store-Load Forwarding (MemorySSA)
//===----------------------------------------------------------------------===//

/// Seed must-alias pairs from MemorySSA store-load forwarding
///
/// This is SOUND: MemorySSA provides precise clobber information.
/// If MemorySSA says a load's clobber is a specific store, then that store
/// is guaranteed to be the only store that could have written to the location.
///
/// Pattern:
///   store %val, %ptr
///   %load = load %ptr  (where MemorySSA says clobber is the store above)
///   → %load must equal %val
void EquivDB::seedStoreLoadForwarding(
    std::vector<std::pair<const Value *, const Value *>> &WL) {
  if (!MSSA)
    return;

  DenseMap<IdTy, const LoadInst *> RepRootToFirstLoad;

  auto mustAliasNow = [&](const Value *A, const Value *B) -> bool {
    if (!A || !B)
      return false;
    if (!A->getType()->isPointerTy() || !B->getType()->isPointerTy())
      return false;
    if (sameSingletonSlot(A, B))
      return true;
    if (atomicMustAlias(DL, A, B))
      return true;

    // Query current equivalence classes as propagation advances.
    id(A);
    id(B);
    return mustAlias(A, B);
  };

  std::function<bool(MemoryAccess *, const LoadInst *,
                     SmallVectorImpl<const Value *> &,
                     SmallPtrSetImpl<const MemoryAccess *> &)>
      collectStoredValues =
          [&](MemoryAccess *Access, const LoadInst *LI,
              SmallVectorImpl<const Value *> &StoredVals,
              SmallPtrSetImpl<const MemoryAccess *> &Visited) -> bool {
    if (!Access || !Visited.insert(Access).second)
      return false;

    if (auto *MUD = dyn_cast<MemoryUseOrDef>(Access)) {
      auto *SI = dyn_cast_or_null<StoreInst>(MUD->getMemoryInst());
      if (!SI)
        return false;
      if (!sameSingletonSlot(SI->getPointerOperand(), LI->getPointerOperand()) &&
          !mustAliasNow(SI->getPointerOperand(), LI->getPointerOperand()))
        return false;

      const Value *StoredVal = SI->getValueOperand();
      if (!StoredVal->getType()->isPointerTy())
        return false;
      StoredVals.push_back(StoredVal);
      return true;
    }

    auto *MP = dyn_cast<MemoryPhi>(Access);
    if (!MP || MP->getNumIncomingValues() == 0)
      return false;

    for (const Use &IncU : MP->incoming_values()) {
      auto *Inc = cast<MemoryAccess>(IncU.get());
      if (!collectStoredValues(Inc, LI, StoredVals, Visited))
        return false;
    }
    return true;
  };

  MemorySSAWalker *Walker = MSSA->getWalker();

  // Walk all instructions in the function
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      // Only interested in load instructions
      auto *LI = dyn_cast<LoadInst>(&I);
      if (!LI)
        continue;

      // Get the memory access for this load
      MemoryUseOrDef *MA = MSSA->getMemoryAccess(LI);
      if (!MA)
        continue;

      MemoryAccess *Clob = nullptr;
      if (Walker)
        Clob = Walker->getClobberingMemoryAccess(MA);
      if (!Clob)
        Clob = MA->getDefiningAccess();

      SmallVector<const Value *, 4> StoredVals;
      SmallPtrSet<const MemoryAccess *, 8> Visited;
      if (!collectStoredValues(Clob, LI, StoredVals, Visited) ||
          StoredVals.empty())
        continue;

      // MemoryPhi merge forwarding: if all reaching stores write one
      // pointer-equivalence class, the load must equal that class.
      const Value *Rep = StoredVals.front();
      bool AllSameClass = true;
      for (const Value *V : StoredVals) {
        if (!mustAliasNow(Rep, V)) {
          AllSameClass = false;
          break;
        }
      }
      if (!AllSameClass)
        continue;

      // SOUND: MemorySSA guarantees the store is the only clobber
      // Therefore, load must equal the stored value
      enqueuePair(WL, LI, Rep);

      // Additional closure: loads forwarding to the same representative class
      // must alias each other.
      const IdTy RepRoot = find(id(Rep));
      auto It = RepRootToFirstLoad.find(RepRoot);
      if (It == RepRootToFirstLoad.end()) {
        RepRootToFirstLoad[RepRoot] = LI;
      } else {
        enqueuePair(WL, LI, It->second);
      }
    }
  }
}

void EquivDB::buildGEPIndex() {
  GEPIndexBuckets.clear();
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB)
      if (const auto *GEP = dyn_cast<GetElementPtrInst>(&I))
        GEPIndexBuckets[gepIndexSignature(GEP)].push_back(GEP);
}

bool EquivDB::equivalentGEPIndexOperand(const Value *A, const Value *B) const {
  if (A == B)
    return true;

  if (A->getType()->isIntegerTy() && B->getType()->isIntegerTy()) {
    A = stripNoopArithmetic(A);
    B = stripNoopArithmetic(B);
    if (A == B)
      return true;
    auto *CA = dyn_cast<ConstantInt>(A);
    auto *CB = dyn_cast<ConstantInt>(B);
    if (CA && CB)
      return CA->getValue() == CB->getValue();
    if (equivalentIntegerExpr(A, B))
      return true;
  }

  return false;
}

uint64_t EquivDB::gepIndexSignature(const Value *GEPV) const {
  auto *GEP = dyn_cast<GEPOperator>(GEPV);
  if (!GEP)
    return 0;

  auto H = hash_value(GEP->getNumOperands());
  for (unsigned i = 1, e = GEP->getNumOperands(); i < e; ++i) {
    const Value *Idx = GEP->getOperand(i);
    if (Idx->getType()->isIntegerTy()) {
      Idx = stripNoopArithmetic(Idx);
      if (const auto *CI = dyn_cast<ConstantInt>(Idx)) {
        H = hash_combine(H, CI->getValue());
      } else if (const auto *Op = dyn_cast<Operator>(Idx)) {
        H = hash_combine(H, Op->getOpcode(), Op->getType());
      } else {
        H = hash_combine(H, Idx->getType());
      }
    } else {
      H = hash_combine(H, Idx);
    }
  }
  return static_cast<uint64_t>(H);
}

bool EquivDB::findClosedGEPCandidate(const GetElementPtrInst *GEPI,
                                     const Value *&Rep) const {
  auto It = GEPIndexBuckets.find(gepIndexSignature(GEPI));
  if (It == GEPIndexBuckets.end())
    return false;

  const Value *BaseI = GEPI->getPointerOperand();
  const unsigned NumOps = GEPI->getNumOperands();

  for (const GetElementPtrInst *GEPJ : It->second) {
    if (GEPJ == GEPI)
      continue;
    if (GEPJ->getNumOperands() != NumOps)
      continue;
    if (!mustAlias(BaseI, GEPJ->getPointerOperand()))
      continue;

    bool SameIndices = true;
    for (unsigned k = 1; k < NumOps; ++k)
      if (!equivalentGEPIndexOperand(GEPI->getOperand(k),
                                     GEPJ->getOperand(k))) {
        SameIndices = false;
        break;
      }

    if (SameIndices) {
      Rep = GEPJ;
      return true;
    }
  }
  return false;
}

bool EquivDB::dominatesInst(const Instruction *Def,
                            const Instruction *Use) const {
  // A store cannot dominate itself as a load — they are never the same object,
  // but guard against the degenerate case anyway.
  if (Def == Use)
    return false;

  if (Def->getParent() != Use->getParent())
    return DT && DT->dominates(Def->getParent(), Use->getParent());

  // Same basic block: Def dominates Use iff Def appears strictly before Use.
  for (const Instruction &I : *Def->getParent()) {
    if (&I == Def)
      return true; // Def comes first → it dominates Use
    if (&I == Use)
      return false; // Use comes first → Def does not dominate Use
  }
  return false;
}

uint64_t EquivDB::makePairKey(const Value *A, const Value *B) {
  IdTy IA = id(A);
  IdTy IB = id(B);
  if (IA > IB)
    std::swap(IA, IB);
  return (static_cast<uint64_t>(IA) << 32) | static_cast<uint64_t>(IB);
}

bool EquivDB::enqueuePair(
    std::vector<std::pair<const Value *, const Value *>> &WL, const Value *A,
    const Value *B) {
  if (!A || !B)
    return false;
  if (!A->getType()->isPointerTy() || !B->getType()->isPointerTy())
    return false;
  uint64_t Key = makePairKey(A, B);
  if (!SeenPairKeys.insert(Key).second)
    return false;
  WL.emplace_back(A, B);
  return true;
}

int EquivDB::resolveReturnedArgNo(const Value *V, const Function *Callee,
                                  unsigned Depth) const {
  if (!V || !Callee || Depth > 8)
    return -1;

  V = stripNoopCasts(V);
  if (const auto *Arg = dyn_cast<Argument>(V))
    return Arg->getParent() == Callee && Arg->getType()->isPointerTy()
               ? static_cast<int>(Arg->getArgNo())
               : -1;

  if (const auto *GEP = dyn_cast<GEPOperator>(V))
    if (GEP->hasAllZeroIndices())
      return resolveReturnedArgNo(GEP->getPointerOperand(), Callee, Depth + 1);

  if (const auto *SI = dyn_cast<SelectInst>(V)) {
    int T = resolveReturnedArgNo(SI->getTrueValue(), Callee, Depth + 1);
    int FNo = resolveReturnedArgNo(SI->getFalseValue(), Callee, Depth + 1);
    return (T >= 0 && T == FNo) ? T : -1;
  }

  if (const auto *PN = dyn_cast<PHINode>(V)) {
    int Common = -1;
    for (const Value *In : PN->incoming_values()) {
      int Cur = resolveReturnedArgNo(In, Callee, Depth + 1);
      if (Cur < 0)
        return -1;
      if (Common < 0)
        Common = Cur;
      else if (Common != Cur)
        return -1;
    }
    return Common;
  }

  if (const auto *Op = dyn_cast<Operator>(V)) {
    if (Op->getOpcode() == Instruction::IntToPtr) {
      const Value *IntOp = stripNoopArithmetic(Op->getOperand(0));
      if (const auto *IntCast = dyn_cast<Operator>(IntOp))
        if (IntCast->getOpcode() == Instruction::PtrToInt)
          return resolveReturnedArgNo(IntCast->getOperand(0), Callee,
                                      Depth + 1);
    }
  }

  return -1;
}

EquivDB::ReturnSummary
EquivDB::summarizeReturnBehavior(const Function *Callee) const {
  ReturnSummary Summary;
  if (!Callee || Callee->isDeclaration() || Callee->empty())
    return Summary;

  bool SawReturn = false;
  for (const BasicBlock &BB : *Callee) {
    const auto *RI = dyn_cast<ReturnInst>(BB.getTerminator());
    if (!RI)
      continue;

    SawReturn = true;
    const Value *RV = RI->getReturnValue();
    if (!RV || !RV->getType()->isPointerTy())
      return ReturnSummary{};

    RV = stripNoopCasts(RV);
    ReturnSummary Cur;

    int ArgNo = resolveReturnedArgNo(RV, Callee);
    if (ArgNo >= 0) {
      Cur.K = ReturnSummary::Kind::Arg;
      Cur.ArgNo = ArgNo;
    } else if (isa<ConstantPointerNull>(RV)) {
      Cur.K = ReturnSummary::Kind::Null;
      Cur.Fixed = RV;
    } else {
      const Value *Obj = getUnderlyingObject(RV);
      if (isa<GlobalValue>(Obj)) {
        Cur.K = ReturnSummary::Kind::Global;
        Cur.Fixed = RV;
      } else {
        return ReturnSummary{};
      }
    }

    if (Summary.K == ReturnSummary::Kind::Unknown) {
      Summary = Cur;
      continue;
    }

    if (Summary.K != Cur.K)
      return ReturnSummary{};

    if (Summary.K == ReturnSummary::Kind::Arg && Summary.ArgNo != Cur.ArgNo)
      return ReturnSummary{};
    if ((Summary.K == ReturnSummary::Kind::Null ||
         Summary.K == ReturnSummary::Kind::Global) &&
        Summary.Fixed != Cur.Fixed)
      return ReturnSummary{};
  }

  if (!SawReturn)
    return ReturnSummary{};
  return Summary;
}
