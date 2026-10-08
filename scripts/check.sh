#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
preset=${1:-debug}
cmake --preset "$preset"
cmake --build --preset "$preset"
if [ "$preset" = tsan ]; then
  setarch x86_64 -R ctest --test-dir build/tsan --output-on-failure
  ZT_BINARY=build/tsan/zt-proxy setarch x86_64 -R python3 -m pytest tests/integration -q
else
  ctest --test-dir "build/$preset" --output-on-failure
  ZT_BINARY="build/$preset/zt-proxy" python3 -m pytest tests/integration -q
fi
