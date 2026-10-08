#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --preset debug
cmake --build --preset debug
mkdir -p bench/results
ZT_RUNNER="valgrind --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=definite,indirect,possible --error-exitcode=99 --log-file=$PWD/bench/results/valgrind-%p.txt" python3 -m pytest tests/integration/test_proxy.py -q -k 'http_and_bodies or tls_download or reload_and_logs or shutdown_drains'
