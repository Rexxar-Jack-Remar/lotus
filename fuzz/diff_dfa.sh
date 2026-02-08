#!/bin/bash
# Differential testing for Dataflow analyses (lib/Dataflow).
# Compares results of overlapping engines (Elimination vs Mono) on the same IR.
# Use with random C → bitcode to find discrepancies between engines.
#
# Usage:
#   ./diff_dfa.sh                    # generate random C with CSmith, compile, diff
#   ./diff_dfa.sh <file.c>           # compile file.c to .bc, run diff
#   ./diff_dfa.sh <file.bc>          # run diff on existing bitcode
#
# Requires: lotus built (bin/lotus-dfa-diff), clang. For random C: CSmith (optional).
# Bitcode must be readable by the same LLVM as lotus (e.g. LLVM 14). If your
# system clang emits opaque-pointer bitcode, set CLANG to that LLVM's clang.

# Uses CSmith to generate random C, compiles to LLVM IR
# export CLANG="/path/to/your/clang"

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
CLANG="${CLANG:-clang}"
OPT="${OPT:-opt}"
CSMITH="${CSMITH:-$BUILD_DIR/csmith-install/bin/csmith}"
CSMITH_HOME="${CSMITH_HOME:-$BUILD_DIR/csmith-install/include}"

# SDK path (macOS)
SDK_PATH=""
if [[ "$OSTYPE" == "darwin"* ]]; then
  SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"
fi

run_diff_test() {
  local BC_FILE="$1"
  if [[ ! -f "$BC_FILE" ]]; then
    echo "error: bitcode file not found: $BC_FILE"
    return 1
  fi

  local DFA_DIFF="$BUILD_DIR/bin/lotus-dfa-diff"
  if [[ ! -x "$DFA_DIFF" ]]; then
    echo "error: lotus-dfa-diff not found. Build with: make lotus-dfa-diff"
    return 1
  fi

  local OUT_DIR="${TMPDIR:-/tmp}/dfa-diff-$$"
  mkdir -p "$OUT_DIR"

  echo "=== Running Elim and Mono liveness on $BC_FILE ==="
  if ! "$DFA_DIFF" --analysis=liveness --engine=both --out-dir="$OUT_DIR" "$BC_FILE" 2>&1; then
    echo "lotus-dfa-diff failed"
    rm -rf "$OUT_DIR"
    return 1
  fi

  if ! diff -q "$OUT_DIR/elim.txt" "$OUT_DIR/mono.txt" >/dev/null 2>&1; then
    echo "DIFF MISMATCH: Elimination and Mono liveness results differ"
    echo "--- elim.txt ---"
    head -50 "$OUT_DIR/elim.txt"
    echo "..."
    echo "--- mono.txt ---"
    head -50 "$OUT_DIR/mono.txt"
    echo "..."
    echo "Full outputs: $OUT_DIR/elim.txt $OUT_DIR/mono.txt"
    rm -rf "$OUT_DIR"
    return 1
  fi

  echo "OK: Elim and Mono liveness results match"
  rm -rf "$OUT_DIR"
  return 0
}

# --- main ---

if [[ $# -ge 1 ]]; then
  INPUT="$1"
  if [[ "$INPUT" == *.bc || "$INPUT" == *.ll ]]; then
    run_diff_test "$INPUT"
    exit $?
  fi
  if [[ "$INPUT" == *.c ]]; then
    BC_FILE="${TMPDIR:-/tmp}/diff_dfa_$$.bc"
    echo "=== Compiling $INPUT to bitcode ==="
    # Use same pointer format as lotus (LLVM 14): try -no-opaque-pointers if supported
    CMD="$CLANG ${SDK_PATH:+-isysroot $SDK_PATH} -w -emit-llvm -c -Xclang -no-opaque-pointers \"$INPUT\" -o \"$BC_FILE\""
    if ! eval "$CMD" 2>&1; then
      CMD="$CLANG ${SDK_PATH:+-isysroot $SDK_PATH} -w -emit-llvm -c \"$INPUT\" -o \"$BC_FILE\""
      if ! eval "$CMD" 2>&1; then
        echo "Compilation failed"
        exit 1
      fi
    fi
    # Run mem2reg + instnamer for stable SSA (lotus-dfa-diff also does this; doing it here keeps IR minimal)
    if command -v "$OPT" &>/dev/null; then
      BC2="${BC_FILE}.opt.bc"
      "$OPT" -mem2reg -instnamer "$BC_FILE" -o "$BC2" 2>/dev/null && mv "$BC2" "$BC_FILE"
    fi
    run_diff_test "$BC_FILE"
    R=$?
    rm -f "$BC_FILE"
    exit $R
  fi
  echo "usage: $0 [file.c|file.bc]"
  exit 1
fi

# No argument: generate random C with CSmith (if available)
C_FILE="${TMPDIR:-/tmp}/diff_dfa_$$.c"
BC_FILE="${TMPDIR:-/tmp}/diff_dfa_$$.bc"

if [[ ! -x "$CSMITH" ]]; then
  echo "CSmith not found at $CSMITH. Provide a .c or .bc file: $0 <file.c|file.bc>"
  exit 1
fi

echo "=== Generating random C: $C_FILE ==="
CSMITH_CMD="$CSMITH --pointers --max-pointer-depth 1 --max-block-size 4"
if ! timeout 15s bash -c "$CSMITH_CMD > \"$C_FILE\" 2>/dev/null"; then
  echo "CSmith failed (timeout or error)"
  rm -f "$C_FILE"
  exit 1
fi

echo "=== Compiling to bitcode ==="
CMD="$CLANG ${SDK_PATH:+-isysroot $SDK_PATH} -I\"$CSMITH_HOME\" -w -emit-llvm -c -Xclang -no-opaque-pointers \"$C_FILE\" -o \"$BC_FILE\""
if ! eval "$CMD" 2>&1; then
  CMD="$CLANG ${SDK_PATH:+-isysroot $SDK_PATH} -I\"$CSMITH_HOME\" -w -emit-llvm -c \"$C_FILE\" -o \"$BC_FILE\""
  if ! eval "$CMD" 2>&1; then
    echo "Compilation failed"
    rm -f "$C_FILE" "$BC_FILE"
    exit 1
  fi
fi

if command -v "$OPT" &>/dev/null; then
  BC2="${BC_FILE}.opt.bc"
  "$OPT" -mem2reg -instnamer "$BC_FILE" -o "$BC2" 2>/dev/null && mv "$BC2" "$BC_FILE"
fi

run_diff_test "$BC_FILE"
R=$?
rm -f "$C_FILE" "$BC_FILE"
exit $R
