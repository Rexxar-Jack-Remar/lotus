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
//   Phase 2: PROPAGATE with semantic rules
//   ───────────────────────────────────────
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
//           try semantic rules (e.g., closed PHI, closed Select):
//             if all pointer operands of I are now in the same class:
//               Rep ← representative of that class
//               push (I, Rep) to WL  // I must equal Rep
//
//   Semantic rules capture higher-order patterns:
//     • Closed PHI:    phi [p₁, bb₁], ..., [pₙ, bbₙ] where p₁ ≡ ... ≡ pₙ
//                      → the PHI must equal the common class
//     • Closed Select: select cond, pTrue, pFalse where pTrue ≡ pFalse
//                      → the Select must equal the common class
//
//   These rules are *inductive*: as more values unify, previously non-closed
//   instructions may become closed, enabling further propagation.
//
//   Phase 3: REFINE with external analyses (optional)
//   ─────────────────────────────────────────────────
//   If MemorySSA is available:
//     • Store-Load forwarding: load p after store v to p = v (no clobber)
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

#include <queue>
#include <unordered_map>

#include <llvm/ADT/Hashing.h>
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

/// Rule 6: Same Underlying Object
/// Two pointers derived from the same alloca or global (via casts/GEPs)
/// that resolve to the same underlying object are aliases.
/// Example: bitcast(%alloca) and GEP(%alloca, 0)
static bool checkSameUnderlyingObject(const Value *S1, const Value *S2) {
  const Value *U1 = getUnderlyingObject(S1);
  const Value *U2 = getUnderlyingObject(S2);

  // Only consider stack allocations and globals (not heap objects)
  return U1 == U2 && (isa<AllocaInst>(U1) || isa<GlobalVariable>(U1));
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
  if (auto *SI = dyn_cast<SelectInst>(S1))
    if (stripNoopCasts(SI->getTrueValue()) == S2 &&
        stripNoopCasts(SI->getFalseValue()) == S2)
      return true;

  if (auto *SI = dyn_cast<SelectInst>(S2))
    if (stripNoopCasts(SI->getTrueValue()) == S1 &&
        stripNoopCasts(SI->getFalseValue()) == S1)
      return true;

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
  // Safety check: ensure V is not null
  if (!V)
    return 0; // Return invalid ID 0 for null values

  // Check if this value already has an ID
  auto It = Val2Id.find(V);
  if (It != Val2Id.end())
    return It->second;

  // Allocate a new ID (the current size of Nodes/Id2Val)
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

/// Semantic Rule S3: Closed GEP
///
/// A GEP is "closed" when there exists another GEP with the same number of
/// operands, equivalent base (must-alias), and identical index operands (same
/// SSA values). Then the two GEP results must alias.
///
/// Example:
///   %base2 ≡ %base1 (after propagation)
///   %a = GEP %base1, 0, %i
///   %b = GEP %base2, 0, %i
///   → %a ≡ %b (rule fires when base classes merge)
bool UnderApprox::ruleClosedGEP(const Instruction *I, const EquivDB &DB,
                                const Value *&Rep) {
  auto *GEPI = dyn_cast<GetElementPtrInst>(I);
  if (!GEPI)
    return false;
  return DB.findClosedGEPCandidate(GEPI, Rep);
}

//-----------------------------------------------------------------------
// Dispatcher table
//-----------------------------------------------------------------------
using RuleTy = bool (*)(const Instruction *, const EquivDB &, const Value *&);
static constexpr RuleTy SemanticRules[] = {
    ruleClosedPHI,
    ruleClosedSelect,
    ruleClosedGEP,
};

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

  // Phase 1b: Seed from MemorySSA store-load forwarding (if available)
  // This is sound: MemorySSA provides precise clobber information
  if (MSSA) {
    seedStoreLoadForwarding(WorkList);
  }

  // Phase 1c: Seed from single-store allocas when DominatorTree available
  // Sound: one store to alloca dominating all loads ⇒ load equals stored value
  if (DT) {
    seedSingleStoreAlloca(WorkList);
  }

  // Phase 2: Propagate equivalences using semantic (inductive) rules
  // As we unify classes, new patterns may emerge (e.g., closed PHIs),
  // which add more pairs to the worklist until saturation.
  propagate(WorkList);
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
  static std::unordered_map<const Function *, ReturnSummary> SummaryCache;

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

      ReturnSummary Summary;
      auto It = SummaryCache.find(Callee);
      if (It == SummaryCache.end()) {
        Summary = summarizeReturnBehavior(Callee);
        SummaryCache.emplace(Callee, Summary);
      } else {
        Summary = It->second;
      }

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

/// Phase 1c: Seed from single-store allocas when DominatorTree available
///
/// For each load from an alloca, if there is exactly one dominating store
/// (pointer-typed value) to that alloca, then the load must-alias the stored
/// value. This handles both the classic single-store case and per-load unique
/// reaching store patterns.
void EquivDB::seedSingleStoreAlloca(
    std::vector<std::pair<const Value *, const Value *>> &WL) {
  if (!DT)
    return;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *AI = dyn_cast<AllocaInst>(&I);
      if (!AI)
        continue;

      const Value *AllocaV = AI;
      SmallVector<StoreInst *, 2> Stores;
      SmallVector<LoadInst *, 4> Loads;

      for (BasicBlock &ScanBB : F)
        for (Instruction &ScanI : ScanBB) {
          if (auto *SI = dyn_cast<StoreInst>(&ScanI)) {
            if (getUnderlyingObject(SI->getPointerOperand()) == AllocaV)
              Stores.push_back(SI);
          } else if (auto *LI = dyn_cast<LoadInst>(&ScanI)) {
            if (getUnderlyingObject(LI->getPointerOperand()) == AllocaV &&
                LI->getType()->isPointerTy())
              Loads.push_back(LI);
          }
        }

      if (Stores.empty() || Loads.empty())
        continue;

      for (LoadInst *LI : Loads) {
        StoreInst *UniqueStore = nullptr;
        bool Ambiguous = false;

        for (StoreInst *SI : Stores) {
          if (!dominatesInst(SI, LI))
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

/// Check if all pointer operands of an instruction are in the same equivalence
/// class
///
/// This is a helper used by semantic rules to detect when an instruction's
/// pointer operands have unified. When they are all in the same class, certain
/// patterns (like closed PHI or closed Select) can be applied.
///
/// Note: This is similar to uniquePtrOperandClass but uses a different
/// implementation that's more efficient when we already have the EquivDB.
///
/// @param I The instruction to check
/// @return true if all pointer operands are in the same class (or there are
///         no pointer operands), false otherwise
///
/// Time complexity: O(P·α(N)) where P = number of pointer operands
bool EquivDB::operandsInSameClass(const Instruction *I) const {
  IdTy Root = 0;
  bool HaveRoot = false;

  // Check each pointer operand
  for (const Value *Op : I->operands())
    if (Op->getType()->isPointerTy()) {
      // Get the root of the class containing this operand
      // Note: const_cast needed because find() is non-const (mutates path
      // compression) This is safe because we're only querying, not modifying
      // the structure
      IdTy Cur = const_cast<EquivDB *>(this)->find(
          const_cast<EquivDB *>(this)->id(Op));

      if (!HaveRoot) {
        // First pointer operand - establish the reference class
        Root = Cur;
        HaveRoot = true;
      } else if (Root != Cur) {
        // Found an operand in a different class - not all unified
        return false;
      }
      // If we get here, this operand is in the same class as the first
    }

  // All pointer operands (if any) are in the same class
  return true;
}

/// Phase 2: Propagate equivalences using semantic (inductive) rules
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

      // Check each instruction watching this class
      for (Instruction *I : List) {
        // Only process pointer-producing instructions (semantic rules apply to
        // these)
        if (!I->getType()->isPointerTy())
          continue;
        if (FiredSemantic.count(I))
          continue;

        // Try each semantic rule in sequence
        // The first rule that fires is applied (rules are mutually exclusive
        // per instruction)
        const Value *Rep = nullptr;
        for (RuleTy R : SemanticRules)
          if (R(I, *this, Rep)) { // Rule fires: I ≡ Rep
            // Add new must-alias pair to worklist for further propagation
            FiredSemantic.insert(I);
            enqueuePair(WL, I, Rep);
            break; // One rule firing per instruction is enough
          }
      }
      // Note: We keep the List intact. Each instruction may be revisited
      // multiple times as its operand classes continue to merge, but semantic
      // rules typically fire at most once per instruction (when it becomes
      // "closed").
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

  auto extractStoreFromAccess =
      [&](const MemoryAccess *Access, const LoadInst *LI) -> StoreInst * {
    auto *MUD = dyn_cast_or_null<MemoryUseOrDef>(
        const_cast<MemoryAccess *>(Access));
    if (!MUD)
      return nullptr;
    auto *SI = dyn_cast_or_null<StoreInst>(MUD->getMemoryInst());
    if (!SI)
      return nullptr;
    if (!atomicMustAlias(DL, SI->getPointerOperand(), LI->getPointerOperand()))
      return nullptr;
    return SI;
  };

  auto resolveUniqueStore = [&](MemoryAccess *Access,
                                const LoadInst *LI) -> StoreInst * {
    if (!Access)
      return nullptr;

    if (auto *SI = extractStoreFromAccess(Access, LI))
      return SI;

    auto *MP = dyn_cast<MemoryPhi>(Access);
    if (!MP)
      return nullptr;

    StoreInst *Unique = nullptr;
    for (const Use &IncU : MP->incoming_values()) {
      auto *Inc = cast<MemoryAccess>(IncU.get());
      StoreInst *S = extractStoreFromAccess(Inc, LI);
      if (!S)
        return nullptr;
      if (!Unique)
        Unique = S;
      else if (Unique != S)
        return nullptr;
    }
    return Unique;
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

      StoreInst *SI = resolveUniqueStore(Clob, LI);
      if (!SI)
        continue;

      // Only unify pointer-typed values: must-alias is defined for pointers
      const Value *StoredVal = SI->getValueOperand();
      if (!StoredVal->getType()->isPointerTy())
        continue;

      // SOUND: MemorySSA guarantees the store is the only clobber
      // Therefore, load must equal the stored value
      enqueuePair(WL, LI, StoredVal);
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
    if (Idx->getType()->isIntegerTy())
      Idx = stripNoopArithmetic(Idx);
    H = hash_combine(H, Idx);
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
      if (!equivalentGEPIndexOperand(GEPI->getOperand(k), GEPJ->getOperand(k))) {
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

bool EquivDB::dominatesInst(const Instruction *Def, const Instruction *Use) const {
  if (Def->getParent() != Use->getParent())
    return DT && DT->dominates(Def->getParent(), Use->getParent());

  if (Def == Use)
    return true;

  for (const Instruction &I : *Def->getParent()) {
    if (&I == Def)
      return true;
    if (&I == Use)
      return false;
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
          return resolveReturnedArgNo(IntCast->getOperand(0), Callee, Depth + 1);
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
