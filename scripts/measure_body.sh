#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
label=${1:-current}
cmake --preset release -DZT_BENCH=ON
cmake --build --preset release
build/release/body_bench > "bench/results/body-$label.json"
valgrind --tool=callgrind --callgrind-out-file="bench/results/callgrind-$label.out" build/release/body_bench 128 > "bench/results/body-profile-$label.json" 2> "bench/results/callgrind-$label.txt"
callgrind_annotate --threshold=95 "bench/results/callgrind-$label.out" > "bench/results/profile-$label.txt"
