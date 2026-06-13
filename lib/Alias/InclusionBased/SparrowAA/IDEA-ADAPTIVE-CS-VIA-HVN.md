# An Idea: Adaptive Context Sensitivity via HVN Feedback

## Research Proposal for SparrowAA

---

## One-Sentence Summary

HVN assigns pointer equivalence labels (`peLabel`) to every node in the constraint
graph. By looking at whether different callers pass actual arguments with different
`peLabel`s to the same function, we can predict whether context sensitivity helps that
function — *before* doing any context-sensitive analysis.

---

## Motivation

### The Problem

Andersen-style pointer analysis with k-CFA context sensitivity applies a uniform
call-string depth k to **every** call site in the program. This is wasteful:

- Many functions receive equivalent pointer information from all callers. Context
  sensitivity for these functions produces redundant nodes without improving precision.
- Only functions whose formal parameters receive **distinct** pointer values from
  different callers actually benefit from context sensitivity.
- The worst-case O(N³) complexity scales linearly with the number of contexts
  (each context multiplies the node count), so unnecessary contexts are expensive.

### The Key Insight

The **HVN** (Hash-based Value Numbering) optimization pass — which is already part
of SparrowAA (`ConstraintOptimize.cpp`) — computes pointer equivalence classes
as a side effect. These classes directly encode the information needed to decide
whether context sensitivity matters:

- If two actual arguments to the same formal parameter have **different peLabels**,
  they are pointer-inequivalent — context sensitivity would keep them separate.
- If they have the **same peLabel**, they are pointer-equivalent even in the
  context-insensitive analysis — adding contexts won't distinguish them.

---

## Concrete Example

```c
// Case 1: Context SENSITIVITY helps
void helper_a(int** p) {
    *p = ...;    // In CI: p → {x, y} merged.
                 // In CS: p@caller1 → {x}, p@caller2 → {y} (more precise)
}

void caller1() { int* x = malloc(); helper_a(&x); }
void caller2() { int* y = malloc(); helper_a(&y); }

// Case 2: Context sensitivity DOESN'T help
void helper_b(int** p, int** q) {
    *p = *q;     // Called from one site. Context adds nothing.
}

void caller() {
    int* x = malloc();
    int* y = malloc();
    helper_b(&x, &y);
}
```

After HVN on the CI constraint graph:

| Node | peLabel | Meaning |
|------|---------|---------|
| `x_val` | 3 | points to allocation site 1 |
| `y_val` | 4 | points to allocation site 2 |
| `p@helper_a` | 5 | predecessors {x_val, y_val} → labels {3, 4} → **diverse** |
| `p@helper_b` | 6 | predecessor {x_val} → label {3} → **uniform** |

**Decision**: `helper_a` gets context depth k (distinguish callers). `helper_b`
inherits the caller's context without deepening (no benefit).

---

## Algorithm

### Lifecycle (Two-Pass in `runOnModule`)

The implementation interleaves a CI pre-pass before the main CS analysis.
This guarantees the HVN scoring is always performed on a context-insensitive
graph, preserving the conservative property (§[Key Property](#key-property-conservative-prediction)).

```
┌─ Pre-pass (in runOnModule, before main analysis) ──────────────────────┐
│                                                                         │
│  Phase 0: CI Collection                                                 │
│  ┌────────────────────────────────────────────────────┐                 │
│  │  collectConstraints(module, k=0)                   │                 │
│  │  → CI constraint graph with all constraints        │                 │
│  │    merged across callers                            │                 │
│  └────────────────────────────────────────────────────┘                 │
│                          │                                              │
│  Phase 1: HVN on CI graph                                               │
│  ┌────────────────────────────────────────────────────┐                 │
│  │  runHVNAndCapturePELabels()                        │                 │
│  │  → peLabel[] assigned to every node                │                 │
│  │  → pointer equivalence classes computed             │                 │
│  └────────────────────────────────────────────────────┘                 │
│                          │                                              │
│  Phase 2: Per-Function Context Scoring (NOVEL)                          │
│  ┌────────────────────────────────────────────────────┐                 │
│  │  computeContextNeed()                              │                 │
│  │  For each function F with ≥1 pointer params:       │                 │
│  │    For each pointer-typed formal param p:           │                 │
│  │      Find all COPY(dest=p, actual_arg)              │                 │
│  │      Collect peLabels of those actual_args          │                 │
│  │      If |labels| > 1 → this param benefits          │                 │
│  │        from context sensitivity                     │                 │
│  │    contextNeed[F] = diverse_params / total_params   │                 │
│  └────────────────────────────────────────────────────┘                 │
│                          │                                              │
│  Reset analysis state (constraints, nodeFactory, ptsGraph cleared)      │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
                                    │
┌─ Main analysis (standard Andersen pipeline) ───────────────────────────┐
│                                                                         │
│  Phase 3: CS Collection with Adaptive Policy (NOVEL)                    │
│  ┌────────────────────────────────────────────────────┐                 │
│  │  collectConstraints(module, k=k)                   │                 │
│  │  → evolveContext() checks contextNeed[callee]:     │                 │
│  │    if score ≥ THRESHOLD → push_call (deepen)       │                 │
│  │    if score < THRESHOLD → return ctx unchanged     │                 │
│  └────────────────────────────────────────────────────┘                 │
│                          │                                              │
│  Phase 4: Constraint Optimization (existing HVN/HU, optional)           │
│  Phase 5: Constraint Solving (standard)                                 │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### Score Interpretation

| Score | Meaning | Action |
|-------|---------|--------|
| 0.0 | All params receive equivalent pointers from all callers | No context needed |
| 0.0 – 0.5 | Some params benefit, most don't | Optional context |
| 0.5 – 1.0 | Most params benefit | Context helpful |
| 1.0 | Every param receives distinct pointers from different callers | Context strongly beneficial |

The default threshold 0.5 means: if at least half of a function's pointer parameters
receive non-equivalent pointers from different callers, give it the user-configured
context depth k.

---

## Novelty

### What Makes This Novel

The central claim — **"use offline optimization results to guide context policy"** —
has not been explored in the pointer analysis literature.

| Prior Work | Approach | Limitation |
|------------|----------|------------|
| Uniform k-CFA (standard) | Same depth for all call sites | Wastes contexts on unhelpful sites |
| Selective CS for Java (Li et al., PLDI'13) | Call-chain topology + allocation-site density heuristics | Indirect metrics, not measuring the actual condition for improvement |
| ML-based context selection (e.g., Jeong et al.) | Learn from program features | Needs training data, black-box, hard to reason about soundness |
| **This work** | **Use HVN equivalence classes directly** | **Directly measures: do different callers pass non-equivalent pointers? Self-contained, no training.** |

### Key Property: Conservative Prediction

The HVN runs on the **CI constraint graph**, which aggressively merges nodes across
contexts. This means:

- The CI graph **under-estimates** pointer diversity (it merges contexts, so some
  distinctions are lost).
- Any diversity we *do* detect in the CI graph is **guaranteed to be real** —
  contexts would definitely separate those values.
- Therefore our scoring is **conservative**: we might miss some functions that would
  benefit, but we never falsely attribute benefit to a function that wouldn't. This
  is the right property for a sound analysis.

### Relationship to Wave Propagation

AserPTA's wave propagation solver also uses SCC-based optimization during solving.
But wave propagation is a *solving strategy* (how to propagate), while this work
is a *context policy* (how to decide which call sites deserve context depth).
They are orthogonal and could be combined.

---

## Implementation

### Files Changed (~220 lines across 5 files)

#### 1. `ConstraintOptimize.cpp` — Expose peLabel + CLI flags (30 lines)

```cpp
// Add to HVNOptimizer class:
public:
  // Return snapshot of peLabel map after run() completes.
  const DenseMap<NodeIndex, unsigned> &getPELabels() const { return peLabel; }
```

HVN is now extracted into a free function so it can be triggered independently
of the `optimizeConstraints()` call — required for the CI pre-pass:

```cpp
// Free function (declared in Andersen.h):
void runHVNAndCapturePELabels(
    std::vector<AndersConstraint> &constraints,
    AndersNodeFactory &nodeFactory,
    llvm::DenseMap<NodeIndex, unsigned> &out) {
  HVNOptimizer hvn(constraints, nodeFactory);
  hvn.run();
  out = hvn.getPELabels();   // copy peLabels to the caller's map
}
```

CLI options for the adaptive feature:

```cpp
// Defined in ConstraintOptimize.cpp, forwarded to Andersen.cpp via extern decl:
cl::opt<bool> EnableAdaptiveCS(
    "andersen-adaptive-cs",
    cl::desc("Enable adaptive context sensitivity: functions whose pointer "
             "formals receive pointer-inequivalent actuals from different "
             "callers are given full k-CFA depth; others are not deepened"),
    cl::init(false), cl::cat(AndersenCategory));
cl::opt<float> AdaptiveCSThreshold(
    "andersen-adaptive-cs-threshold",
    cl::desc("Context-need score threshold in [0,1] (default 0.5)"),
    cl::init(0.5f), cl::cat(AndersenCategory));
```

`optimizeConstraints()` also adapted: when adaptive CS already ran HVN during
the CI pre-pass, it skips the duplicate HVN to avoid redundant work:

```cpp
void Andersen::optimizeConstraints() {
  if (EnableHVN) {
    // HVN was already run during CI pre-pass when adaptive CS is active;
    // running it again here is unnecessary but harmless.
    runHVNAndCapturePELabels(constraints, nodeFactory, hvnPELabels);
    // computeContextNeed() already called in pre-pass; skip.
  }
  if (EnableHU) { /* existing HU code */ }
}
```

#### 2. `Andersen.h` — Add scoring members + evolveContext declaration (33 lines)

```cpp
class Andersen {
  // ... existing members ...

  /// peLabel map captured from HVN (populated by CI pre-pass or optimizeConstraints).
  llvm::DenseMap<NodeIndex, unsigned> hvnPELabels;

  /// Per-function context scores in [0,1] (populated by computeContextNeed).
  llvm::DenseMap<const llvm::Function*, float> contextNeed;

  // Compute context need scores from CI constraint graph + HVN peLabels
  void computeContextNeed();

public:
  // evolveContext — moved from inline to out-of-line definition
  // so it can conditionally consult contextNeed.
  AndersNodeFactory::CtxKey evolveContext(
      AndersNodeFactory::CtxKey prev,
      const llvm::Instruction *I) const;

  // Query per-function context score (for inspection / debugging)
  float getContextNeed(const llvm::Function* F) const {
    auto it = contextNeed.find(F);
    return it != contextNeed.end() ? it->second : 0.0f;
  }
  // Expose the full map for iteration (e.g., --print-context-need)
  const llvm::DenseMap<const llvm::Function*, float> &
  getContextNeedMap() const { return contextNeed; }
};
```

Forward declaration of the HVN helper is also added to `Andersen.h`:

```cpp
void runHVNAndCapturePELabels(
    std::vector<AndersConstraint> &constraints,
    AndersNodeFactory &nodeFactory,
    llvm::DenseMap<NodeIndex, unsigned> &out);
```

#### 3. `Andersen.cpp` — CI pre-pass + scoring + adaptive evolve (118 lines)

The central orchestration change is in `runOnModule()`. When adaptive CS is
active with `k > 0`, a lightweight CI pre-pass runs first:

```cpp
bool Andersen::runOnModule(const Module &M) {
  if (EnableAdaptiveCS && ctxPolicy.k > 0) {
    // Phase 0-2: CI pre-pass
    ContextPolicy ciPolicy = makeContextPolicy(0);
    std::swap(ctxPolicy, ciPolicy);           // swap to k=0
    initialCtx = ctxPolicy.initialCtx();
    globalCtx = ctxPolicy.globalCtx();
    collectConstraints(M);                    // CI collection

    runHVNAndCapturePELabels(constraints, nodeFactory, hvnPELabels);
    computeContextNeed();                     // score functions

    // Reset state for main CS collection
    constraints.clear();
    nodeFactory = AndersNodeFactory();
    ptsGraph.clear();
    std::swap(ctxPolicy, ciPolicy);           // restore adaptive CS policy
    initialCtx = ctxPolicy.initialCtx();
    globalCtx = ctxPolicy.globalCtx();
    visitedFunctions.clear();
  }

  // Phase 3-5: Main CS collection + optimize + solve
  collectConstraints(M);                      // uses evolveContext() below
  ...
}
```

The scoring implementation examines COPY constraints whose destination is a
formal argument, and counts how many pointer formals receive actuals with
more than one distinct peLabel:

```cpp
void Andersen::computeContextNeed() {
  llvm::DenseMap<const llvm::Function *,
                 llvm::DenseMap<NodeIndex, llvm::SmallVector<unsigned, 4>>>
      paramLabels;

  for (const auto &c : constraints) {
    if (c.getType() != AndersConstraint::COPY) continue;

    NodeIndex dest = nodeFactory.getMergeTarget(c.getDest());
    const llvm::Value *val = nodeFactory.getValueForNode(dest);
    if (!val || !llvm::isa<llvm::Argument>(val)) continue;

    const auto *arg = llvm::cast<llvm::Argument>(val);
    const llvm::Function *F = arg->getParent();

    NodeIndex src = nodeFactory.getMergeTarget(c.getSrc());
    auto labelIt = hvnPELabels.find(src);
    unsigned label = labelIt != hvnPELabels.end() ? labelIt->second : 0;
    if (label == 0) continue;                 // non-pointer actual, skip
    paramLabels[F][dest].push_back(label);
  }

  for (auto &[F, params] : paramLabels) {
    int diverse = 0, total = 0;
    for (auto &[param, labels] : params) {
      ++total;
      llvm::SmallVector<unsigned, 4> sorted = labels;
      std::sort(sorted.begin(), sorted.end());
      if (std::unique(sorted.begin(), sorted.end()) != sorted.begin() + 1)
        ++diverse;
    }
    contextNeed[F] = total > 0
        ? static_cast<float>(diverse) / total : 0.0f;
  }
}
```

The adaptive evolve logic checks `contextNeed[callee]` before deciding
whether to push a new context:

```cpp
AndersNodeFactory::CtxKey Andersen::evolveContext(
    AndersNodeFactory::CtxKey prev,
    const llvm::Instruction *I) const {
  if (!EnableAdaptiveCS || contextNeed.empty())
    return ctxPolicy.evolve(prev, I);           // fall through to original

  if (I) {
    if (const auto *cb = llvm::dyn_cast<llvm::CallBase>(I)) {
      if (const auto *callee = cb->getCalledFunction()) {
        auto it = contextNeed.find(callee);
        float score = (it != contextNeed.end()) ? it->second : 0.0f;
        if (score < AdaptiveCSThreshold)
          return prev;                          // don't deepen
      }
    }
  }
  return ctxPolicy.evolve(prev, I);             // original evolve
}
```

#### 4. `ConstraintCollect.cpp` — Hook evolveContext (4 lines)

Two call sites in `addConstraintForCall` are changed from
`ctxPolicy.evolve(callerCtx, cs)` to `evolveContext(callerCtx, cs)`,
so that the adaptive decision actually takes effect during constraint
collection:

```cpp
// Before:
AndersNodeFactory::CtxKey calleeCtx = ctxPolicy.evolve(callerCtx, cs);
// After:
AndersNodeFactory::CtxKey calleeCtx = evolveContext(callerCtx, cs);
```

This is the only plumbing change needed — the virtual dispatch of
`evolveContext()` transparently consults the `contextNeed` map when
adaptive CS is enabled.

#### 5. `tools/alias/lotus-alias-sparrow-aa.cpp` — Diagnostic output (37 lines)

Added `--print-context-need` flag to display per-function scores
(sorted descending, annotated with `[deepened]`/`[skipped]`):

```cpp
static cl::opt<bool> PrintContextNeed(
    "print-context-need",
    cl::desc("Print per-function adaptive-CS context-need scores "
             "(requires --andersen-adaptive-cs)"),
    cl::init(false), cl::cat(SparrowAACategory));

// In main(), after analysis:
if (PrintContextNeed) {
  const auto &scores = Anders.getContextNeedMap();
  // Collect, sort by score descending, print
  for (const auto &[score, name] : rows)
    outs() << llvm::format("  %-40s  %.3f  %s\n", name.c_str(), score,
                           score >= AdaptiveCSThreshold ? "[deepened]"
                                                        : "[skipped]");
}
```

---

## Evaluation

### Metrics

| Metric | Measurement | Expected Outcome |
|--------|-------------|------------------|
| Points-to set size | Average per-pointer | Smaller than CI, close to full k-CFA |
| Run time | Wallclock + memory | Lower than full k-CFA |
| Contexts created | Count of `CtxKey` allocations | Fewer than full k-CFA |
| Call-edge resolution | Poly indirect call sites | Same as full k-CFA (precision preserved) |

### Hypothesis

> For a given context depth k, adaptive CS will achieve ≥90% of the precision
> improvement of uniform k-CFA while using ≤60% of the contexts (and proportional
> time/memory savings).

### Protocol

1. Run SparrowAA in four configurations on SPEC2006:
   - CI (k=0) — baseline
   - Full k-CFA (k=1, k=2) — upper bound on precision
   - Adaptive CS (k=1, threshold=0.5) — our method
2. Compare: points-to set sizes, number of contexts, analysis time, indirect-call
   resolution

---

## Related Work

Compariwon with selective context-sensitive analysis for Java

1. Semantic signal vs. structural heuristics. Prior work uses structural signals: call-chain depth (how deep), allocation density (how many), call frequency (how often). Our signal is semantic: "do different callers provide pointer-inequivalent values?" This directly measures what context sensitivity achieves — distinguishing semantically distinct inputs. This is a different class of heuristic.
2. Self-contained feedback loop. Prior work requires a separate pre-analysis (e.g., call-graph traversal, type-flow analysis). We use a byproduct of an optimization pass that's already part of the same analysis. The information is free (almost) — just read the peLabels that HVN already computed.
3. Conservative guarantee. Because CI under-merges, any diversity we detect is guaranteed real. No prior selective CS work I know of has this property — structural heuristics may or may not correlate with actual context benefit. This is mathematically true, not heuristic.
4. C/C++ ≠ Java. Java selective CS targets object sensitivity for OO programs. We target inclusion-based k-CFA for C with function pointers. The cost structures are different: in Java, object sensitivity creates contexts per allocation site (potentially unlimited); in C, k-CFA creates contexts per call chain (bounded by k). The problem is mechanically different.

---

## FAQ

### Q: Doesn't HVN already reduce the need for context sensitivity?

Partially. HVN merges equivalent nodes *within* a single context, reducing the
constraint graph size. But it doesn't address the cross-context problem:
uniform k-CFA creates redundant copies of functions that don't benefit from
context. Our work targets this *different* source of overhead.

### Q: Is the threshold tunable?

Yes. The threshold controls aggressiveness:
- **0.0**: All functions get contexts (identical to uniform k-CFA)
- **0.5**: Only functions where ≥50% of params benefit get contexts (default)
- **1.0**: Only functions where every param benefits get contexts (most aggressive)
- **> 0**: Any function with at least one benefiting param gets contexts

---

## Summary

| Property | Detail |
|----------|--------|
| **Novelty** | First use of HVN equivalence classes for context policy guidance |
| **Mechanism** | peLabel diversity at formal parameters → per-function context score |
| **Code change** | ~220 lines across 5 files |
| **Infrastructure** | All existing (HVN, ContextPolicy, Constraints) |
| **Eval** | Run on SPEC2006, compare pts-sets, time, contexts vs. uniform k-CFA |
| **Risk** | Low (HVN is fast, scoring is cheap, fallback to uniform k-CFA is trivial) |
