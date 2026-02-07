#!/bin/bash
# for testing whether crashes occur in the PTA analysis.
# export CLANG="/path/to/your/clang"
CLANG="${CLANG:-clang}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_ROOT/build"
CSMITH="$BUILD_DIR/csmith-install/bin/csmith"
CSMITH_HOME="$BUILD_DIR/csmith-install/include"

# Get SDK path only on macOS
SDK_PATH=""
if [[ "$OSTYPE" == "darwin"* ]]; then
    SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || true)"
fi

while true; do
    C_FILE="$SCRIPT_DIR/test_$$.c"
    BC_FILE="$SCRIPT_DIR/test_$$.bc"

    # Generate random C program
    echo "=== Generating C file: $C_FILE ==="
    CSMITH_CMD="$CSMITH --pointers --structs --unions --arrays --volatile-pointers --const-pointers --jumps --embedded-assigns"
    CSMITH_CMD="$CSMITH_CMD --max-pointer-depth $((RANDOM % 3 + 1))"
    CSMITH_CMD="$CSMITH_CMD --max-struct-fields $((RANDOM % 8 + 3))"
    CSMITH_CMD="$CSMITH_CMD --max-union-fields $((RANDOM % 5 + 2))"
    CSMITH_CMD="$CSMITH_CMD --max-expr-complexity $((RANDOM % 10 + 5))"
    CSMITH_CMD="$CSMITH_CMD --max-block-depth $((RANDOM % 3 + 2))"
    CSMITH_CMD="$CSMITH_CMD --max-block-size $((RANDOM % 3 + 2))"

    if ! timeout 10s bash -c "$CSMITH_CMD > \"$C_FILE\" 2>/dev/null"; then
        echo "✗ Failed to generate C file (timeout or error)"
        continue
    fi
    echo "✓ C file generated"
    
    # Compile to LLVM IR
    echo "=== Compiling to LLVM IR: $BC_FILE ==="
    CMD="$CLANG ${SDK_PATH:+-isysroot \"$SDK_PATH\"} -I\"$CSMITH_HOME\" -w -emit-llvm -c \"$C_FILE\" -o \"$BC_FILE\""
    echo "Command: $CMD"
    if ! eval "$CMD" 2>&1; then
        echo "Output: Compilation failed"
        rm -f "$C_FILE"
        continue
    fi
    echo "Output: Compilation successful"
    
    # Run analysis tools
    # sparrow-aa with different --andersen-k-cs values
    for k_cs in 0 1 2; do
        echo "=== Running sparrow-aa with --andersen-k-cs=$k_cs ==="
        if ! "$BUILD_DIR/bin/sparrow-aa" --andersen-k-cs="$k_cs" "$BC_FILE" 2>&1; then
            echo "CRASH: sparrow-aa (--andersen-k-cs=$k_cs) crashed on $C_FILE"
            echo "Test files preserved: $C_FILE, $BC_FILE"
            exit 1
        fi
        echo "✓ sparrow-aa (--andersen-k-cs=$k_cs) completed successfully"
    done
    
    # aser-aa with different --analysis-mode values
    for mode in ci 1-cfa 2-cfa origin; do
        echo "=== Running aser-aa with --analysis-mode=$mode ==="
        if ! "$BUILD_DIR/bin/aser-aa" --analysis-mode="$mode" "$BC_FILE" 2>&1; then
            echo "CRASH: aser-aa (--analysis-mode=$mode) crashed on $C_FILE"
            echo "Test files preserved: $C_FILE, $BC_FILE"
            exit 1
        fi
        echo "✓ aser-aa (--analysis-mode=$mode) completed successfully"
    done
    
    # aser-aa with different --analysis-mode values and --pta-use-bdd-pts
    for mode in ci 1-cfa 2-cfa origin; do
        echo "=== Running aser-aa with --analysis-mode=$mode --pta-use-bdd-pts ==="
        if ! "$BUILD_DIR/bin/aser-aa" --analysis-mode="$mode" --pta-use-bdd-pts "$BC_FILE" 2>&1; then
            echo "CRASH: aser-aa (--analysis-mode=$mode --pta-use-bdd-pts) crashed on $C_FILE"
            echo "Test files preserved: $C_FILE, $BC_FILE"
            exit 1
        fi
        echo "✓ aser-aa (--analysis-mode=$mode --pta-use-bdd-pts) completed successfully"
    done
    
    # lotus-aa (no variants)
    echo "=== Running lotus-aa ==="
    if ! "$BUILD_DIR/bin/lotus-aa" "$BC_FILE" 2>&1; then
        echo "CRASH: lotus-aa crashed on $C_FILE"
        echo "Test files preserved: $C_FILE, $BC_FILE"
        exit 1
    fi
    echo "✓ lotus-aa completed successfully"
    
    # tpa with different -k-limit values
    for k_limit in 0 1 2; do
        echo "=== Running tpa with -k-limit=$k_limit ==="
        if ! "$BUILD_DIR/bin/tpa" -k-limit "$k_limit" "$BC_FILE" 2>&1; then
            echo "CRASH: tpa (-k-limit=$k_limit) crashed on $C_FILE"
            echo "Test files preserved: $C_FILE, $BC_FILE"
            exit 1
        fi
        echo "✓ tpa (-k-limit=$k_limit) completed successfully"
    done
    
    # Cleanup if no crash
    rm -f "$C_FILE" "$BC_FILE"
done