#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
preset=${1:-debug}
cmake --preset "$preset"
cmake --build --preset "$preset"
ctest --test-dir "build/$preset" --output-on-failure
ZT_BINARY="build/$preset/zt-proxy" python3 -m pytest tests/integration -q
