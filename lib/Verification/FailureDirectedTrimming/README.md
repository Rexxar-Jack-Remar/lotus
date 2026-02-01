# Failure-Directed Program Trimming (FDTrim)

This directory implements **failure-directed program trimming** as an LLVM IR instrumentation pass, based on:

- Kostas Ferles, Valentin Wüstholz, Maria Christakis, Isil Dillig. *Failure-Directed Program Trimming*. ESEC/FSE 2017.

The goal is to produce a program `P'` that is **equi-safe** with the original `P` (paper §3, Definition: equi-safety; Def. *Trimmed program*): `P'` has an assertion violation iff `P` has one, while pruning many paths that are provably irrelevant to failures.

## Key ideas (paper §1, §4)

- A **safety condition** `SC(π)` at a program point `π` is a *sufficient* condition for executions starting at `π` to avoid assertion failure (paper: φ ⇒ wp(s, true); under the tool's termination model).
- A **trimming condition** `TC(π)` is obtained as `¬SC(π)` (paper §5). Since `SC(π)` is sufficient for safety, `¬SC(π)` is a necessary condition for failure; instrumenting `assume(¬SC(π))` can only remove safe paths (Theorem 5.1).
- The safety conditions computed here are intentionally **stronger than necessary** (an under-approximation of the safe states) so they can be inferred cheaply and still prune many paths.

## What the pass actually does (paper §5 Program Instrumentation)

At a high level ([Pass.cpp]):

1. **Interprocedural transformation (paper §5, Interprocedural instrumentation)**:
   - Clone each eligible function `f` into a *safe clone* `f.fdtrim.safe`.
     - In the safe clone, `assert(c)` is rewritten to `assume(c)` and `error()` is rewritten to `assume(false)` so the clone cannot exhibit assertion failure.
     - Calls inside safe clones are rewired to other safe clones.
   - Wrap each direct call `call f(args)` in the original program into a nondet split:
     - **success branch** calls `f.fdtrim.safe(args)`;
     - **failure branch** calls the original `f(args)` and then executes `assume(false); unreachable`.
   - Intuition: every execution in the transformed program either (a) follows "safe behavior" through safe clones or (b) explicitly enters a "failure context". This makes it sound to compute and insert trimming conditions locally while still preserving failing behaviors.

2. **Compute safety conditions** (paper §4, Figure 3 rules (1)–(10)) with a lightweight backward analysis over the CFG ([SafetyConditions.cpp]).

3. **Insert trimming assumptions** (paper §5, Intraprocedural instrumentation):
   - Choose instrumentation points (calls / conditionals / loop headers) depending on options (paper §6, Bounding the instrumentation).
   - For each point `π`, obtain `SC(π)` from the analysis, form `TC(π) = ¬SC(π)`, simplify/limit it (e.g. bound conjuncts; paper §6), and insert `verifier.assume(TC(π))` before `π`.

## Semantics of the computed formulas

The analysis computes formulas in a small AST ([FailureDirectedTrimmingImpl.h] representing (typed) boolean and integer expressions, pointer dereferences, and quantifiers:

- `BeforeInst[I]` is a **sufficient safety condition** for the execution starting *at instruction `I`* to avoid assertion failure (assuming the execution terminates according to the IR model used by the verifier).
- `PreAfterPhi[BB]` is the safety condition **at the block entry after PHIs** (used for edge transfer through PHI substitution).
- `Summary` is `PreAfterPhi[entry]` and is used as the function's safety-condition summary for calls.

These safety conditions are an under-approximation of safe states (i.e., they may be too strong). Negating them produces trimming conditions that may be too weak (i.e., may not prune as much) but remain sound for equi-safety when instrumented as `assume`.

## Transfer functions and how they relate to the paper (Figure 3, §4.2)

The analysis is a backward condition inference (judgment Λ, Υ, Φ ⊢ s : Φ') reminiscent of weakest-precondition rules but deliberately approximate:

- **Rule (7) Assertions / Rule (8) Assumptions / errors**:
  - `assert(p)` → Φ' = p ∧ Φ (paper rule (7)).
  - `assume(p)` → Φ' = p ⇒ Φ (paper rule (8)).
  - `error()` → Φ' = false (no safe state can reach error).
- **Rule (2) Assignments / pure instructions**:
  - Substitute RHS expressions into Φ when representable (Φ[e/v]); otherwise conservatively havoc the result (paper rule (2)).
- **Rule (3) Heap read**: v₁ := *v₂ → Φ[drf(v₂)/v₁].
- **Rule (4) Heap write**: paper *store*(drf(α), e, Λ, Φ) = Φ[e/drf(α)] ∧ ⋀_{αᵢ∈A\{α}} αᵢ≠α, A = aliases(α)∩derefs(Φ); impl. `storeOp` / `storeTransfer`.
- **Rule (5) Malloc**: Φ' = ∀v. Φ (havoc assigned variable).
- **Rule (6) Procedure calls**: Φ_s = ∀v. havoc(ᾱ, Φ), Φ' = Φ_s ∧ summary(prc, Υ, v_act); paper Def. *Procedure summary* and Def. *Havoc operation*. We approximate modLocs (ᾱ) by havocking derefs in Φ that may be modified (onlyAccessesArgMemory heuristic).

The main implementation points are:
- heap store: `storeOp` / `storeTransfer` (paper Def. *Store operation*)
- call: `callTransfer` / `summaryOf` (paper Def. *Procedure summary*, *Havoc operation*)

## Quantifiers: why they appear and how they are handled

The analysis uses `forall` to represent **havoc** (universal quantification over a fresh value), matching the paper's presentation. When we negate a safety condition to obtain a trimming condition, quantifiers flip (`¬∀x.φ` becomes `∃x.¬φ`). This introduces `exists` binders, which must be eliminated to generate executable `assume` code.

The instrumentation pipeline handles existentials in three steps:

1. Negate the safety condition (`negateForTrimming`, which applies De Morgan and flips quantifiers).
2. Optionally use Z3 quantifier elimination when enabled (`fdtrim-qe=z3`).
3. Replace remaining `exists` by **nondeterministic witnesses** (`eliminateExistsByNondet`) and substitute those into the formula.

As a last-resort safety measure, if quantifiers still reach code generation, they are treated as `true` (i.e., no pruning) so that the inserted assumptions remain a necessary condition for failure.

## Options (selected) (paper §6 Implementation)

Options are declared in [Options.cpp]

- `-fdtrim-instrument-calls / -conditionals / -loops`: where to insert trimming assumes.
- `-fdtrim-summary-iterations`: bounded refinement of summaries (not a full SCC fixpoint).
- `-fdtrim-cfg-iterations`: bounded per-function CFG iteration for safety condition propagation.
- `-fdtrim-max-conjuncts`: limit size of inserted conditions.
- `-fdtrim-qe={nondet,z3}` and `-fdtrim-qe-timeout-ms`: existential elimination strategy.
- `-fdtrim-deref-mode={uf,load,nondet}`: how `drf(ptr)` terms are lowered into LLVM IR.
- `-fdtrim-model-ub-ops`: model div/rem/shifts instead of havocing them (off by default).

## Important limitations / assumptions

- The analysis assumes the verifier's termination model: executions terminate on `assert`/`assume` violations; trimming does not preserve non-termination behaviors.
- Safe cloning currently excludes functions containing `invoke`, `callbr`, inline asm, and indirect calls (or direct calls with mismatched function types).
- Indirect calls and address-taken functions limit where safe clones can be used; the pass may conservatively skip instrumentation for such functions.
- Alias and mod/ref reasoning is approximate and depends on the configured alias-analysis backend.
