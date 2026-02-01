#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="${ROOT}/build"
cd "${BUILD}/tests" && ctest -R "_test$" --output-on-failure
