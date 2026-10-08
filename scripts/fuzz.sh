#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --preset fuzz
cmake --build --preset fuzz
mkdir -p build/fuzz/corpus/{http,sni} bench/results
cp tests/fuzz/corpus/http/* build/fuzz/corpus/http/
cp tests/fuzz/corpus/sni/* build/fuzz/corpus/sni/
for name in http sni; do
  setarch x86_64 -R build/fuzz/fuzz_$name "build/fuzz/corpus/$name" -max_total_time="${1:-60}" -timeout=5 -max_len=20000 -print_final_stats=1 >"bench/results/fuzz-$name.txt" 2>&1
done
