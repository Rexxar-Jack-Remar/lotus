# Under-Approximation Alias Analysis

Sound but incomplete alias analysis for identifying **must-alias** relationships.

## Overview

This analysis computes pointer equivalence using union-find plus congruence over
**normalized pointer terms**. If two pointers are in the same equivalence class,
they are **guaranteed** to alias (sound under-approximation). We never produce
false positives, but may miss some true aliases.

## Algorithm

### Phase 1: SEED with Atomic Rules
Scan all instructions and apply local syntactic rules to identify must-alias
pairs. Each matching pair is added to a worklist. Pointer-producing
instructions also register watches on operand classes and emit normalized term
keys for congruence.

### Phase 2: PROPAGATE with Normalized Terms
Process the worklist, unifying equivalence classes. When classes merge, revisit
watched instructions and rebuild their normalized terms:

- closed `phi`/`select` nodes collapse to the common operand class
- `gep` terms are matched by base-class plus normalized offset/index structure
- congruent terms are unified through a lightweight term table

## Data Structures

- **Val2Id**: `DenseMap<const Value*, IdTy>` - value → ID mapping
- **Id2Val**: `vector<const Value*>` - ID → value mapping  
- **Nodes**: `vector<{Parent, Rank}>` - union-find forest with path compression
- **Watches**: `vector<SmallVector<Inst*>>` - per-class watch lists for incremental updates
- **TermBuckets**: normalized term → representative candidates for congruence
- **SlotId**: singleton memory slot `(object, constant byte offset)` for
  alloca/global/direct-allocation load-store reasoning

## Files

- `UnderApproxAA.cpp` - AAResult interface, per-function caching
- `EquivDB.cpp` - Equivalence database with union-find
- `Canonical.cpp` - Pointer canonicalization helpers
- `AliasGraph.cpp` / `AliasGraph.h` - CC'18 alias graph data structure (see below)

## AliasGraph (Separate Prototype)

`AliasGraph.cpp` / `AliasGraph.h` implement a CC'18-style access-path
must-alias graph. That structure is tested independently, but it is **not**
the engine queried by `UnderApproxAA` today. `UnderApproxAA` is the lightweight
term-and-slot analysis described in this document.

## Rules

### Atomic Rules (Phase 1)
Local, syntactic patterns applied during seeding:

| # | Rule Name | Description | Example |
|---|-----------|-------------|---------|
| 1 | Identity | Same SSA value | `%p` and `%p` |
| 2 | Cast Equivalence | Bitcast or no-op addrspace cast | `%p` and `bitcast %p to i8*` |
| 3 | Constant Offset GEP | Same base + identical offsets | `GEP(%base, 0, i)` and `GEP(%base, 0, i)` |
| 4 | Zero GEP | All-zero indices same as base | `GEP(%p, 0, 0)` and `%p` |
| 4b | Same GEP operands | Same base and identical index operands (SSA) | `GEP(%base, i)` and `GEP(%base, i)` |
| 5 | Round-Trip Cast | ptr→int→ptr with no arithmetic | `inttoptr(ptrtoint(%p))` and `%p` |
| 6 | Same Underlying Object | Same alloca/global via casts/GEPs | `bitcast(%alloca)` and `GEP(%alloca, 0)` |
| 7 | Constant Null | Null pointers in same address space | `null` and `null` |
| 8 | Trivial PHI | All incoming values are identical | `phi [%p, %bb1], [%p, %bb2]` and `%p` |
| 9 | Trivial Select | Both branches produce same value | `select %c, %p, %p` and `%p` |
| 10 | Same Allocation Site | Same malloc/new call | `%p = malloc(); %q = bitcast %p` → `%p ≡ %q` |
| 11 | Enhanced Round-Trip | ptr→int→ptr with no-op arithmetic | `inttoptr(ptrtoint(%p) + 0)` and `%p` |

### Normalized Term Rules (Phase 2)
Congruence patterns checked during propagation:

| Rule | Description | When It Fires |
|------|-------------|---------------|
| **Closed PHI** | `phi [p₁, bb₁], ..., [pₙ, bbₙ]` where `p₁ ≡ ... ≡ pₙ` | PHI normalizes to the common pointer class |
| **Closed Select** | `select cond, pTrue, pFalse` where `pTrue ≡ pFalse` | Select normalizes to the common pointer class |
| **Congruent Constant GEP** | Same constant offset from must-alias bases | Rebuilt term key matches another `gep` term |
| **Congruent Indexed GEP** | Same source element type and equivalent index expressions from must-alias bases | Rebuilt term key matches another `gep` term |

### Extension Rules (Phase 3 - Optional)

| Rule | Description | Requirement |
|------|-------------|-------------|
| **Return-Value Forwarding** | `call/invoke/callbr` result forwarded from pointer argument | `returned` attribute, wrapper-style return expression resolves to one arg, or callee summary |
| **Store-Load Forwarding** | `store v, p; load q` → load equals `v` when `p` and `q` name the same singleton slot or MemorySSA proves one unique clobber | MemorySSA walker plus singleton-slot abstraction |
| **Singleton-Slot Forwarding** | For each load, unique dominating pointer store to same singleton slot → load equals stored value | DominatorTree with `SlotId = (alloca/global/direct-allocation, constant offset)` |

## Query Interface

After construction, query must-alias relationships in **O(α(N))** time:

```cpp
// Basic usage
EquivDB db(function);
bool result = db.mustAlias(pointerA, pointerB);
// result = true  → A and B are guaranteed to alias
// result = false → unknown (may or may not alias)

// With optional analyses (UnderApproxAA interface)
UnderApproxAA AA(module);
AA.setMemorySSAProvider([](Function &F) -> MemorySSA* {
  return &getAnalysis<MemorySSAAnalysis>(F).getMSSA();
});
AA.setDominatorTreeProvider([](Function &F) -> DominatorTree* {
  return &getAnalysis<DominatorTreeAnalysis>(F).getDomTree();
});
bool result = AA.mustAlias(pointerA, pointerB);
```

## Adding New Rules

### Adding an Atomic Rule

1. Create checker in `EquivDB.cpp`:
```cpp
/// Rule X: Brief Description
/// Example: concrete LLVM IR
static bool checkNewRule(const Value *S1, const Value *S2) {
  // Return true if must-alias, false otherwise
}
```

2. Call from `atomicMustAlias()`:
```cpp
if (checkNewRule(S1, S2))  return true;
```

3. Update atomic rules table above

**Requirements**: Sound (no false positives), local, syntactic, fast

### Adding a Semantic Rule

1. Create rule function:
```cpp
static bool ruleNewPattern(const Instruction *I,
                          const EquivDB &DB, const Value *&Rep) {
  // Check if pattern applies and all operands are in same class
  // Set Rep to the representative value
  // Return true if rule fires
}
```

2. Add to `SemanticRules[]` table

3. Update semantic rules table above

**Requirements**: Check `DB.mustAlias()` for operand classes, set `Rep` on success

## Example

### Simple Case: Atomic Rules Only
```llvm
%a = alloca i32
%b = bitcast i32* %a to i8*
%p = getelementptr i8, i8* %b, 0
```

**Phase 1 (Seed):**
- Rule 2: `%a ≡ %b` (bitcast)
- Rule 4: `%b ≡ %p` (zero-offset GEP)

**Phase 2 (Propagate):** Unify classes → `%a ≡ %b ≡ %p`

**Query:** `mustAlias(%a, %p)` → **true**

### Complex Case: Semantic Rules
```llvm
entry:
  %x = alloca i32
  br i1 %cond, label %then, label %else
then:
  %y1 = bitcast i32* %x to i8*    ; %x ≡ %y1
  br label %merge
else:
  %y2 = bitcast i32* %x to i8*    ; %x ≡ %y2
  br label %merge
merge:
  %phi = phi i8* [%y1, %then], [%y2, %else]
  %z = select i1 %cond, i8* %phi, i8* %phi
```

**Phase 1 (Seed):**
- Rule 2: `%x ≡ %y1`, `%x ≡ %y2`
- Watch: `%phi` watches classes of `%y1` and `%y2`
- Watch: `%z` watches class of `%phi`

**Phase 2 (Propagate):**
1. Unify `%x ≡ %y1` → revisit `%phi`
2. Unify `%x ≡ %y2` → now `%y1 ≡ %y2`!
3. **Closed PHI rule fires**: all operands of `%phi` are in same class
   - Add `(%phi, %x)` to worklist
4. Unify `%phi ≡ %x` → revisit `%z`
5. **Closed Select rule fires**: both branches of `%z` are `%phi`
   - Add `(%z, %phi)` to worklist
6. Unify `%z ≡ %phi`

**Result:** `%x ≡ %y1 ≡ %y2 ≡ %phi ≡ %z`

**Query:** `mustAlias(%x, %z)` → **true**

## Contract

- **Intra-procedural only**: no whole-program memory state or cross-function
  fixpoint
- **Path-insensitive**: merges control flow conservatively
- **Must-alias only**: `true` means same concrete address is proven;
  `false` means unknown
- **Field-sensitive only for constant paths**: via normalized constant-offset
  or indexed `gep` terms
- **Memory-sensitive only for singleton slots**: allocas, globals, and direct
  allocation results with constant byte offsets
- **Lightweight interprocedural support**: direct-call return forwarding and
  simple summaries, not general heap effect summaries

## Implemented Extensions (Sound)

The following extensions have been implemented while preserving under-approximation:

- **Same Allocation Site**: Pointers from same `malloc`/`new`/`calloc` call are unified
- **Enhanced Round-Trip**: `inttoptr(ptrtoint(p) + 0)` patterns detected
- **Return-Value Forwarding**: Supports `CallBase`, `returned` attributes, and multi-return same-arg callees
- **Interprocedural Return Summaries**: Tracks `ret ≡ arg_i`, `ret ≡ null`, and fixed global-return forms for direct callees
- **Store-Load Forwarding**: Via MemorySSA walker with unique-store extraction (including simple `MemoryPhi` cases)
- **Alloca Forwarding**: Per-load unique dominating store forwarding via DominatorTree
- **Normalized GEP Terms**: congruence over constant-offset and indexed GEP
  terms, guarded by source-element-type checks
- **Worklist Dedup**: Pair deduplication prevents redundant propagation churn

## Future Improvement Ideas

Additional sound extensions that could be added:

- **Cache invalidation**: Add `invalidateCache(F)` for passes that modify IR

## Performance

- **Construction**: O(N·α(N)) where N = number of values, α is inverse Ackermann
- **Query**: O(α(N)) ≈ O(1) in practice
- **Memory**: O(N) for union-find + watch lists

## Related Work

- CC 18: An Efficient Data Structure for Must-Alias Analysis. https://yanniss.github.io/must-datastruct-cc18.pdf
- ICTAC 12: Definite expression aliasing analysis for Java bytecode.
- ISoLA 08:  Computing must and may alias to detect null pointer dereference.
- ISSTA 06: Effective typestate verification in the presence of aliasing.
- TOPLAS 02: Parametric shape analysis via 3-valued logic.
- TOPLAS 99: Interprocedural pointer alias analysis
- POPL 98: Single and loving it: Must-alias analysis for higher-order languages
- POPL 95: An extended form of must alias analysis for dynamic allocation. 
- PLDI 94: Context-sensitive interprocedural points-to analysis in the presence of function pointers. I
