# Failure-Directed Program Trimming (FDTrim)

This directory implements **failure-directed program trimming** as an LLVM IR instrumentation pass, based on:

- Kostas Ferles, Valentin Wüstholz, Maria Christakis, Isil Dillig. *Failure-Directed Program Trimming*. ESEC/FSE 2017. (See [trimming.md](file:///Users/rainoftime/Work/analysis/lotus/trimming.md))

The goal is to produce a program `P'` that is **equi-safe** with the original `P` (it has an assertion violation iff `P` has one) while pruning many paths that are provably irrelevant to failures.

## Key ideas

- A **safety condition** `SC(π)` at a program point `π` is a *sufficient* condition for executions starting at `π` to avoid assertion failure (under the tool's termination model).
- A **trimming condition** `TC(π)` is obtained as `¬SC(π)`. Since `SC(π)` is sufficient for safety, `¬SC(π)` is a necessary condition for failure; instrumenting `assume(¬SC(π))` can only remove safe paths.
- The safety conditions computed here are intentionally **stronger than necessary** (an under-approximation of the safe states) so they can be inferred cheaply and still prune many paths.

## What the pass actually does

At a high level ([Pass.cpp](file:///Users/rainoftime/Work/analysis/lotus/lib/Verification/FailureDirectedTrimming/Pass.cpp)):

1. **Interprocedural transformation (modularity trick)** ([CloneAndWrap.cpp](file:///Users/rainoftime/Work/analysis/lotus/lib/Verification/FailureDirectedTrimming/CloneAndWrap.cpp)):
   - Clone each eligible function `f` into a *safe clone* `f.fdtrim.safe`.
     - In the safe clone, `assert(c)` is rewritten to `assume(c)` and `error()` is rewritten to `assume(false)` so the clone cannot exhibit assertion failure.
     - Calls inside safe clones are rewired to other safe clones.
   - Wrap each direct call `call f(args)` in the original program into a nondet split:
     - **success branch** calls `f.fdtrim.safe(args)`;
     - **failure branch** calls the original `f(args)` and then executes `assume(false); unreachable`.
   - Intuition: every execution in the transformed program either (a) follows "safe behavior" through safe clones or (b) explicitly enters a "failure context". This makes it sound to compute and insert trimming conditions locally while still preserving failing behaviors.

2. **Compute safety conditions** with a lightweight backward analysis over the CFG ([SafetyConditions.cpp](file:///Users/rainoftime/Work/analysis/lotus/lib/Verification/FailureDirectedTrimming/SafetyConditions.cpp)).

3. **Insert trimming assumptions**:
   - Choose instrumentation points (calls / conditionals / loop headers) depending on options.
   - For each point `π`, obtain `SC(π)` from the analysis, form `TC(π) = ¬SC(π)`, simplify/limit it, and insert `verifier.assume(TC(π))` before `π`.

## Semantics of the computed formulas

The analysis computes formulas in a small AST ([FailureDirectedTrimmingImpl.h](file:///Users/rainoftime/Work/analysis/lotus/lib/Verification/FailureDirectedTrimming/FailureDirectedTrimmingImpl.h)) representing (typed) boolean and integer expressions, pointer dereferences, and quantifiers:

- `BeforeInst[I]` is a **sufficient safety condition** for the execution starting *at instruction `I`* to avoid assertion failure (assuming the execution terminates according to the IR model used by the verifier).
- `PreAfterPhi[BB]` is the safety condition **at the block entry after PHIs** (used for edge transfer through PHI substitution).
- `Summary` is `PreAfterPhi[entry]` and is used as the function's safety-condition summary for calls.

These safety conditions are an under-approximation of safe states (i.e., they may be too strong). Negating them produces trimming conditions that may be too weak (i.e., may not prune as much) but remain sound for equi-safety when instrumented as `assume`.

## Transfer functions and how they relate to the paper

The analysis is a backward condition inference reminiscent of weakest-precondition rules but deliberately approximate:

- **Assertions / errors**:
  - `assert(p)` strengthens the safety condition with `p` (a failing execution must satisfy `p` at that point).
  - `error()` makes the safety condition `false` (no safe state can reach error).
- **Assumptions**:
  - `assume(p)` yields `p ⇒ Φ` (if `p` holds, the rest must be safe; otherwise the execution ends with assumption violation, which is not considered a failure).
- **Assignments / pure instructions**:
  - Substitute RHS expressions into `Φ` when representable; otherwise conservatively "havoc" the result.
- **Heap reads/writes**:
  - Loads are modeled as `drf(ptr)` terms.
  - Stores use a conservative `store()` rule that replaces matching `drf(ptr)` terms and adds alias-disambiguation constraints against other dereference locations already mentioned in `Φ`.
- **Procedure calls**:
  - Conjoin the (possibly substituted) callee summary.
  - Havoc the return value, and havoc dereference locations that appear in the current formula and may be modified by the call (heuristics around `onlyAccessesArgMemory`).

The main implementation points are:
- heap store modeling: `storeOp` / `storeTransfer`
- call modeling: `callTransfer` / `summaryOf`

## Quantifiers: why they appear and how they are handled

The analysis uses `forall` to represent **havoc** (universal quantification over a fresh value), matching the paper's presentation. When we negate a safety condition to obtain a trimming condition, quantifiers flip (`¬∀x.φ` becomes `∃x.¬φ`). This introduces `exists` binders, which must be eliminated to generate executable `assume` code.

The instrumentation pipeline handles existentials in three steps:

1. Negate the safety condition (`negateForTrimming`, which applies De Morgan and flips quantifiers).
2. Optionally use Z3 quantifier elimination when enabled (`fdtrim-qe=z3`).
3. Replace remaining `exists` by **nondeterministic witnesses** (`eliminateExistsByNondet`) and substitute those into the formula.

As a last-resort safety measure, if quantifiers still reach code generation, they are treated as `true` (i.e., no pruning) so that the inserted assumptions remain a necessary condition for failure.

## Options (selected)

Options are declared in [Options.cpp](file:///Users/rainoftime/Work/analysis/lotus/lib/Verification/FailureDirectedTrimming/Options.cpp).

- `-fdtrim-instrument-calls / -conditionals / -loops`: where to insert trimming assumes.
- `-fdtrim-summary-iterations`: bounded refinement of summaries (not a full SCC fixpoint).
- `-fdtrim-cfg-iterations`: bounded per-function CFG iteration for safety condition propagation.
- `-fdtrim-max-conjuncts`: limit size of inserted conditions.
- `-fdtrim-qe={nondet,z3}` and `-fdtrim-qe-timeout-ms`: existential elimination strategy.
- `-fdtrim-deref-mode={uf,load,nondet}`: how `drf(ptr)` terms are lowered into LLVM IR.

## Important limitations / assumptions

- The analysis assumes the verifier's termination model: executions terminate on `assert`/`assume` violations; trimming does not preserve non-termination behaviors.
- Safe cloning currently excludes functions containing `invoke`, `callbr`, inline asm, and indirect calls (or direct calls with mismatched function types).
- Indirect calls and address-taken functions limit where safe clones can be used; the pass may conservatively skip instrumentation for such functions.
- Alias and mod/ref reasoning is approximate and depends on the configured alias-analysis backend.

