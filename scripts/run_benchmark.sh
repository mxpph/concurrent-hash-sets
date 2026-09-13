#!/usr/bin/env bash
cmake -G "Unix Makefiles" . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-stdlib=libc++"
cmake --build build --config Release --target benchmarks --parallel

taskset -c 0-15 ./build/benchmarks \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true \
  "$@"
