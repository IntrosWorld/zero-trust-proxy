# Reproduce measurements

```bash
cmake --preset release -DZT_BENCH=ON
cmake --build --preset release
ZT_BINARY=build/release/zt-proxy python3 bench/run.py --seconds 30
python3 bench/plot.py                 # needs matplotlib
python3 bench/summarize.py            # updates README from saved data
ZT_BINARY=build/release/zt-proxy python3 bench/observe.py
python3 tests/integration/redteam.py
bash scripts/valgrind.sh
bash scripts/fuzz.sh 30
bash scripts/measure_body.sh current
```

`run.py` starts a bounded-memory asyncio HTTP backend. wrk sends either direct origin-form requests or absolute-form proxy requests through a generated Lua script. Both cases close each connection after one response, so keepalive does not bias the direct comparison. All traffic stays on loopback. Authentication is disabled, rate limits lifted, fake DNS enabled, and access logs sent to `/dev/null` for this benchmark. These are controlled performance settings, not the shipped production configuration. Real disk logging, DNS and remote latency can change results.

The default matrix is 100 and 1,000 concurrent clients, with 1, 2, 4 and the machine's logical CPU count in workers. Each wrk run lasts 30 seconds with four load-generator threads. Files preserve requests/sec, p50/p99, socket errors, HTTP errors, a live metrics scrape and an `ss` snapshot. `environment.json` records toolchain, kernel and CPU information. `http.json` summarizes the raw files; `scaling.svg` shows total rate and rate per worker.

The TLS benchmark hashes a 500 MiB deterministic stream through a real TLS connection. Its byte count and SHA-256 must match the expected stream. CPU percentage is proxy user+system CPU time divided by wall time relative to one core, using `/proc/PID/stat`. Hashing, the Python TLS backend and the client share the host with the proxy; the measurement is not an Internet speed test.

The idle test opens 1,000 unfinished header connections and reports RSS before/after; it measures the allocated connection objects and two relay buffers, not 1,000 completed TLS sessions. Kernel socket buffers are outside the reported process RSS. The half-open red-team check independently opens 2,000 sockets and verifies per-client limits and another source IP's ability to obtain a response.

`body_bench` isolates Content-Length framing bookkeeping. Callgrind showed the byte scan was the dominant cost; the final implementation counts whole blocks. The saved before/after timings and instruction counts are **not network bandwidth**. To reproduce the pre-optimization behavior, use `src/http/request_parser.cpp` from commit `226e7ea` in a disposable checkout, with the current benchmark driver and CMake option. Avoid replacing files in a working tree with unrelated edits.

The first `perf record` attempt was denied by this host's `perf_event_paranoid=4`; `perf-attempt.txt` preserves the failure. Callgrind profiles and annotated hotspots are provided instead of a fabricated perf flame graph. Optional splice and seccomp experiments are not implemented.

The red-team runner reports **scenario categories**, not independent exploit probabilities. R6/R7 and several policy subcases share fixtures, but each assertion checks its own result. R5 models rebinding by changing a fake DNS mapping after a connection is pinned; its initial endpoint is explicitly permitted loopback test infrastructure, not a public IP routed locally. L3 integration smoke is 1 MiB; the separate TLS benchmark supplies the full 500 MiB check. No categories are silently skipped.

## Kubernetes measurements

`k8s/run.py` covers E1/E2/E5–E10; `k8s/scaling.py` covers E3/E4 and Redis latency. Run them sequentially against the dedicated cluster. Raw JSON, sanitizer/static-analysis results, pod security settings and live dashboard/plots are under `results/k8s/`. `k8s/plot.py` recreates the plots from E3/E4 JSON using matplotlib. See the [Kubernetes guide](../docs/kubernetes.md) for exact commands, policy restoration, measured results and limitations. High-rate requests intentionally receive authenticated HTTP 403 policy decisions; only E8's functional check contacts the external origin. Do not compare those decision rates directly with the original forwarding benchmark above.
