#!/usr/bin/env bash
set -eu

COMPILER="${COMPILER:-clang}"
case "${COMPILER}" in
  clang|clang++)
    CXX_COMPILER="clang++"
    CXX_FLAGS="-stdlib=libc++"
    ;;
  gcc|g++)
    CXX_COMPILER="g++"
    CXX_FLAGS=""
    ;;
  *)
    echo "usage: $0 [clang|gcc]" >&2
    exit 1
    ;;
esac

BUILD_DIR="build/${COMPILER}"
cmake -G "Unix Makefiles" . -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="${CXX_COMPILER}" \
  -DCMAKE_CXX_FLAGS="${CXX_FLAGS}"
cmake --build "${BUILD_DIR}" --config Release --target benchmarks --parallel

taskset -c 0-15 "./${BUILD_DIR}/benchmarks" \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true \
  "$@"
