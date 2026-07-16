#!/usr/bin/env bash
# Build Lotus with its supported LLVM 14 and Z3 dependencies.
#
# Common examples:
#   ./build.sh
#   ./build.sh debug
#   ./build.sh clean debug -j 8
#   ./build.sh --test
#   ./build.sh --no-tests -- -DLOTUS_ENABLE_CLAM=OFF
#   LLVM_BUILD_PATH=/path/to/llvm/lib/cmake/llvm Z3_DIR=/path/to/z3 ./build.sh

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)

BUILD_TYPE="Release"
BUILD_DIR="${LOTUS_BUILD_DIR:-}"
BUILD_DIR_EXPLICIT=false
JOBS="${LOTUS_BUILD_JOBS:-${CMAKE_BUILD_PARALLEL_LEVEL:-}}"
GENERATOR="${LOTUS_GENERATOR:-${CMAKE_GENERATOR:-}}"
RUN_BUILD=true
RUN_TESTS=false
RUN_INSTALL=false
CLEAN=false
TESTS_OPTION=""
EXAMPLES_OPTION=""

CMAKE_ARGS=()
TARGETS=()

usage() {
  cat <<'USAGE'
Usage: ./build.sh [release|debug] [clean] [options] [-- CMake arguments]

Build modes:
  release                 Configure a Release build (default).
  debug                   Configure a Debug build.
  clean                   Remove the selected build directory before configuring.

Options:
  -j, --jobs N            Number of parallel build jobs.
  -B, --build-dir DIR     Build directory (default: build or build-debug).
  -G, --generator NAME    CMake generator, for example Ninja.
  -t, --target NAME       Build a specific target; may be repeated.
      --tests             Enable Lotus unit tests at configure time.
      --no-tests          Disable Lotus unit tests at configure time.
      --examples          Enable examples at configure time.
      --no-examples       Disable examples at configure time.
      --test              Run CTest after building.
      --install           Build the install target after the normal build.
      --configure-only    Configure without building.
      --cmake-arg ARG     Pass one argument directly to CMake; may be repeated.
  -h, --help              Show this help message.

Arguments beginning with -D or -U are also passed directly to CMake. All arguments
after -- are forwarded to the CMake configure command.

Environment variables:
  LLVM_BUILD_PATH         LLVM 14 build prefix or directory containing LLVMConfig.cmake.
  LLVM_DIR                Directory containing LLVMConfig.cmake.
  Z3_DIR                  Z3 installation prefix.
  LOTUS_BUILD_DIR         Override the default build directory.
  LOTUS_BUILD_JOBS        Override the default parallelism.
  LOTUS_GENERATOR         Override the CMake generator.
  CMAKE_BUILD_PARALLEL_LEVEL
                          Fallback parallelism when LOTUS_BUILD_JOBS is unset.

The script discovers package-manager installations but does not install dependencies.
USAGE
}

die() {
  echo "error: $*" >&2
  exit 1
}

require_value() {
  local option="$1"
  local value="${2:-}"
  [[ -n "$value" ]] || die "$option requires a value"
}

default_jobs() {
  local detected=""

  if command -v nproc >/dev/null 2>&1; then
    detected=$(nproc)
  elif [[ "$(uname -s)" == "Darwin" ]] && command -v sysctl >/dev/null 2>&1; then
    detected=$(sysctl -n hw.ncpu 2>/dev/null || true)
  elif command -v getconf >/dev/null 2>&1; then
    detected=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
  fi

  echo "${detected:-4}"
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      [Dd]ebug)
        BUILD_TYPE="Debug"
        ;;
      [Rr]elease)
        BUILD_TYPE="Release"
        ;;
      [Cc]lean|--clean)
        CLEAN=true
        ;;
      -j|--jobs)
        require_value "$1" "${2:-}"
        JOBS="$2"
        shift
        ;;
      -j[0-9]*)
        JOBS="${1#-j}"
        ;;
      -B|--build-dir)
        require_value "$1" "${2:-}"
        BUILD_DIR="$2"
        BUILD_DIR_EXPLICIT=true
        shift
        ;;
      -G|--generator)
        require_value "$1" "${2:-}"
        GENERATOR="$2"
        shift
        ;;
      -t|--target)
        require_value "$1" "${2:-}"
        TARGETS+=("$2")
        shift
        ;;
      --tests)
        TESTS_OPTION="ON"
        ;;
      --no-tests)
        TESTS_OPTION="OFF"
        ;;
      --examples)
        EXAMPLES_OPTION="ON"
        ;;
      --no-examples)
        EXAMPLES_OPTION="OFF"
        ;;
      --test)
        RUN_TESTS=true
        TESTS_OPTION="ON"
        ;;
      --install)
        RUN_INSTALL=true
        ;;
      --configure-only)
        RUN_BUILD=false
        ;;
      --cmake-arg)
        require_value "$1" "${2:-}"
        CMAKE_ARGS+=("$2")
        shift
        ;;
      -D*|-U*)
        CMAKE_ARGS+=("$1")
        ;;
      --)
        shift
        while [[ $# -gt 0 ]]; do
          CMAKE_ARGS+=("$1")
          shift
        done
        break
        ;;
      -h|--help|[Hh]elp)
        usage
        exit 0
        ;;
      *)
        die "unknown argument '$1' (run ./build.sh --help for usage)"
        ;;
    esac
    shift
  done
}

find_llvm_cmake_dir() {
  local llvm_config=""
  local candidate=""

  for llvm_config in llvm-config-14 llvm-config; do
    if command -v "$llvm_config" >/dev/null 2>&1; then
      if [[ "$("$llvm_config" --version 2>/dev/null || true)" == 14.* ]]; then
        candidate=$("$llvm_config" --cmakedir 2>/dev/null || true)
        if [[ -f "$candidate/LLVMConfig.cmake" ]]; then
          echo "$candidate"
          return 0
        fi
      fi
    fi
  done

  if command -v brew >/dev/null 2>&1; then
    candidate="$(brew --prefix llvm@14 2>/dev/null || true)/lib/cmake/llvm"
    if [[ -f "$candidate/LLVMConfig.cmake" ]]; then
      echo "$candidate"
      return 0
    fi
  fi

  for candidate in \
    /opt/homebrew/opt/llvm@14/lib/cmake/llvm \
    /usr/local/opt/llvm@14/lib/cmake/llvm \
    /usr/lib/llvm-14/lib/cmake/llvm \
    /usr/local/llvm-14/lib/cmake/llvm; do
    if [[ -f "$candidate/LLVMConfig.cmake" ]]; then
      echo "$candidate"
      return 0
    fi
  done

  return 1
}

is_z3_root() {
  local root="$1"
  local library=""

  [[ -f "$root/include/z3++.h" ]] || return 1
  for library in "$root"/lib/libz3.* "$root"/bin/libz3.*; do
    if [[ -f "$library" ]]; then
      return 0
    fi
  done
  return 1
}

find_z3_root() {
  local candidate=""

  if command -v brew >/dev/null 2>&1; then
    candidate=$(brew --prefix z3 2>/dev/null || true)
    if [[ -n "$candidate" ]] && is_z3_root "$candidate"; then
      echo "$candidate"
      return 0
    fi
  fi

  if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists z3; then
    candidate=$(pkg-config --variable=prefix z3 2>/dev/null || true)
    if [[ -n "$candidate" ]] && is_z3_root "$candidate"; then
      echo "$candidate"
      return 0
    fi
  fi

  for candidate in /opt/homebrew/opt/z3 /usr/local/opt/z3 /usr/local /usr; do
    if is_z3_root "$candidate"; then
      echo "$candidate"
      return 0
    fi
  done

  return 1
}

cmake_definition_value() {
  local name="$1"
  local argument=""
  local value=""
  local found=false

  for argument in "${CMAKE_ARGS[@]}"; do
    if [[ "$argument" == "-D${name}="* || \
          "$argument" == "-D${name}:"*"="* ]]; then
      value="${argument#*=}"
      found=true
    fi
  done

  [[ "$found" == true ]] || return 1
  echo "$value"
}

configure_dependencies() {
  local discovered=""
  local cmake_llvm_build_path=""
  local cmake_llvm_dir=""
  local cmake_z3_dir=""

  cmake_llvm_build_path=$(cmake_definition_value LLVM_BUILD_PATH || true)
  cmake_llvm_dir=$(cmake_definition_value LLVM_DIR || true)
  cmake_z3_dir=$(cmake_definition_value Z3_DIR || true)

  if [[ -n "$cmake_llvm_build_path" ]]; then
    LLVM_BUILD_PATH="$cmake_llvm_build_path"
  elif [[ -n "${LLVM_BUILD_PATH:-}" ]]; then
    [[ -d "$LLVM_BUILD_PATH" ]] || die "LLVM_BUILD_PATH is not a directory: $LLVM_BUILD_PATH"
    CMAKE_ARGS+=("-DLLVM_BUILD_PATH=$LLVM_BUILD_PATH")
  fi

  if [[ -n "$cmake_llvm_dir" ]]; then
    LLVM_DIR="$cmake_llvm_dir"
  elif [[ -n "${LLVM_DIR:-}" ]]; then
    [[ -d "$LLVM_DIR" ]] || die "LLVM_DIR is not a directory: $LLVM_DIR"
    CMAKE_ARGS+=("-DLLVM_DIR=$LLVM_DIR")
  fi

  if [[ -z "${LLVM_BUILD_PATH:-}" && -z "${LLVM_DIR:-}" ]]; then
    if discovered=$(find_llvm_cmake_dir); then
      LLVM_DIR="$discovered"
      CMAKE_ARGS+=("-DLLVM_DIR=$LLVM_DIR")
    else
      die "LLVM 14 was not found; set LLVM_BUILD_PATH or LLVM_DIR"
    fi
  fi

  if [[ -n "$cmake_z3_dir" ]]; then
    Z3_DIR="$cmake_z3_dir"
  elif [[ -n "${Z3_DIR:-}" ]]; then
    [[ -d "$Z3_DIR" ]] || die "Z3_DIR is not a directory: $Z3_DIR"
    CMAKE_ARGS+=("-DZ3_DIR=$Z3_DIR")
  elif discovered=$(find_z3_root); then
    Z3_DIR="$discovered"
    CMAKE_ARGS+=("-DZ3_DIR=$Z3_DIR")
  else
    die "Z3 was not found; install it or set Z3_DIR to its installation prefix"
  fi
}

clean_build_dir() {
  local absolute_build_dir=""

  [[ -e "$BUILD_DIR" ]] || return 0
  [[ -d "$BUILD_DIR" ]] || die "build path is not a directory: $BUILD_DIR"

  absolute_build_dir=$(cd -- "$BUILD_DIR" 2>/dev/null && pwd -P) || \
    die "cannot resolve build directory: $BUILD_DIR"

  [[ "$absolute_build_dir" != "/" ]] || die "refusing to remove /"
  [[ "$absolute_build_dir" != "$SCRIPT_DIR" ]] || \
    die "refusing to remove the source directory"

  if [[ -e "$BUILD_DIR" ]]; then
    echo "Removing build directory: $BUILD_DIR"
    rm -rf -- "$BUILD_DIR"
  fi
}

print_config() {
  echo "Lotus build configuration:"
  echo "  Source:       $SCRIPT_DIR"
  echo "  Build:        $BUILD_DIR"
  echo "  Type:         $BUILD_TYPE"
  echo "  Generator:    ${GENERATOR:-CMake default}"
  echo "  Jobs:         $JOBS"
  echo "  LLVM:         ${LLVM_BUILD_PATH:-${LLVM_DIR:-CMake search path}}"
  echo "  Z3:           ${Z3_DIR:-CMake search path}"
  echo "  Build tests:  ${TESTS_OPTION:-CMake default}"
  echo "  Examples:     ${EXAMPLES_OPTION:-CMake default}"
}

configure_lotus() {
  local command=(cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR")

  command+=("-DCMAKE_BUILD_TYPE=$BUILD_TYPE")
  [[ -z "$GENERATOR" ]] || command+=(-G "$GENERATOR")
  [[ -z "$TESTS_OPTION" ]] || command+=("-DLOTUS_BUILD_TESTS=$TESTS_OPTION")
  [[ -z "$EXAMPLES_OPTION" ]] || command+=("-DLOTUS_BUILD_EXAMPLES=$EXAMPLES_OPTION")
  command+=("${CMAKE_ARGS[@]}")

  echo "Configuring Lotus..."
  "${command[@]}"
}

build_lotus() {
  local command=(cmake --build "$BUILD_DIR")

  if [[ ${#TARGETS[@]} -gt 0 ]]; then
    command+=(--target "${TARGETS[@]}")
  fi
  command+=(-- -j "$JOBS")

  echo "Building Lotus..."
  "${command[@]}"
}

test_lotus() {
  echo "Running Lotus tests..."
  (cd -- "$BUILD_DIR" && ctest --output-on-failure -j "$JOBS")
}

install_lotus() {
  echo "Installing Lotus..."
  cmake --build "$BUILD_DIR" --target install -- -j "$JOBS"
}

main() {
  command -v cmake >/dev/null 2>&1 || die "cmake is required"

  parse_args "$@"
  JOBS="${JOBS:-$(default_jobs)}"
  [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "jobs must be a positive integer: $JOBS"

  if [[ -z "$BUILD_DIR" ]]; then
    if [[ "$BUILD_TYPE" == "Debug" ]]; then
      BUILD_DIR="$SCRIPT_DIR/build-debug"
    else
      BUILD_DIR="$SCRIPT_DIR/build"
    fi
  elif [[ "$BUILD_DIR_EXPLICIT" == false && "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$SCRIPT_DIR/$BUILD_DIR"
  fi

  if [[ "$RUN_BUILD" == false && ( "$RUN_TESTS" == true || "$RUN_INSTALL" == true ) ]]; then
    die "--configure-only cannot be combined with --test or --install"
  fi

  [[ "$CLEAN" == false ]] || clean_build_dir
  configure_dependencies
  print_config
  configure_lotus

  if [[ "$RUN_BUILD" == true ]]; then
    build_lotus
    [[ "$RUN_TESTS" == false ]] || test_lotus
    [[ "$RUN_INSTALL" == false ]] || install_lotus
  fi
}

main "$@"
