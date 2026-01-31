# Sifa verifier tool

Command-line entrance for **Sifa** (Symbolic Interpretation with Fluid Abstractions) from `lib/Verification/Sifa`.

- **Default:** Instruction-by-instruction transfer (Interval or Octagon domain), **no SMT solver** — fast.
- **--symabs:** SymbolicAbstraction-backed domain (SMT solver) — more precise but slower.

## Build

From the repo root:

```bash
cmake --build build --target sifa
```

## Usage

```bash
sifa <bitcode.bc> [options]
```

- **&lt;bitcode.bc&gt;** – LLVM bitcode file (e.g. from `clang -c -emit-llvm` then `opt -mem2reg -instnamer`).
- **--function &lt;name&gt;** – Function to analyze (default: `main` or first defined).
- **--block &lt;label&gt;** – Basic block label to analyze to (default: analyze to return).
- **--abstract-domain &lt;domain&gt;** – Domain: `Interval` (default) or `Octagon`. With **--symabs**: `Interval`, `Octagon`, or `Interval, Octagon`.
- **--symabs** – Use SymbolicAbstraction backend (SMT solver). Omit for instruction-by-instruction transfer (no SMT).
- **--reachability** – Only report reachability (no abstract state).
- **--progress** – Print progress messages while analyzing.
- **--no-validate-subset** – Disable strict IR subset checks (may crash on unsupported IR).
- **--list-functions** – List functions in the module.
- **--list-blocks** – List basic blocks in the selected function.
- **--verbose** – Print abstract state (with PrettyPrinter).
