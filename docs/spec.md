# ZT-Proxy — High-Performance Zero Trust Forward Proxy in C++ (Linux)

**Platform:** Ubuntu (your dual boot)
**Language:** Modern C++ (C++20), Linux system APIs
**Suggested location:** `~/projects/zt-proxy` (keep it on the Ubuntu ext4 partition, not the NTFS drive, for correct file permissions and build speed)
**Suggested GitHub repo:** `IntrosWorld/zt-proxy`

**One-line pitch:** An event-driven HTTP/HTTPS forward proxy for Linux that enforces Zero Trust egress policy (domain allowlisting, TLS SNI verification, DNS-pinned SSRF protection, rate limiting) without decrypting traffic, benchmarked for throughput and latency and hardened with sanitizers and fuzzing.

> **How to use this file:** in a Claude Code session on Ubuntu, in `~/projects/zt-proxy`, say:
> *"Implement the spec in `zt-proxy-spec.md`, milestone by milestone. Stop after each milestone, show me the test results, and explain the code before moving on."*
> Copy this file into the project folder first. Stopping after each milestone matters: you will have to explain every part of this code in interviews.

---

## 1. Why this project

### What it proves for Zscaler
Zscaler's core product (**ZIA, Zscaler Internet Access**) is a security proxy that all user traffic passes through. Their engineering job ads repeatedly ask for **C/C++, Linux, network programming, concurrency, performance, debugging (GDB/Valgrind)**. This project is a small version of that product, built with that stack.

| Zscaler requirement | Where this project proves it |
|---|---|
| C/C++ (production quality) | Modern C++20, RAII, no raw `new`/`delete`, sanitizer-clean |
| Linux / POSIX | `epoll`, non-blocking sockets, `eventfd`, signals, `systemd` |
| Network programming | TCP client and server, HTTP/1.1 parsing, `CONNECT` tunnels, DNS |
| SSL/TLS | Parsing the TLS ClientHello to read SNI (no decryption) |
| Proxy architecture | It *is* a forward proxy |
| Concurrency / multithreading | One event loop per core, a resolver thread pool, lock-free counters |
| High performance / low latency | Measured requests/second, p50/p99 latency, tunnel throughput |
| Debugging | GDB, Valgrind, AddressSanitizer / UBSan / ThreadSanitizer, fuzzing |
| Zero Trust / security | Deny-by-default policy, SSRF and DNS-rebinding protection, SNI mismatch detection |
| Observability | Prometheus-style metrics endpoint and structured access logs |

### How it fits your other projects
- **SentinelAgent** (`F:\College\AIML_project`) routes all its outbound traffic through this proxy, so the agent is secured by it (§12).
- **The malware platform** is your threat-detection project; this one is your network-security project.

---

## 2. Ubuntu setup

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build gdb valgrind clang clang-tidy clang-format \
    lld curl openssl python3 python3-venv python3-pip wrk apache2-utils tcpdump \
    linux-tools-common linux-tools-$(uname -r)
```

- **Unit tests:** GoogleTest, fetched by CMake's `FetchContent` (no system install needed).
- **Integration tests:** Python 3 + `pytest` in a virtual environment (they drive real sockets, `curl` and `openssl`).
- **Record the versions in the README:** `lsb_release -a`, `g++ --version`, `clang++ --version`, `cmake --version`, and the CPU model and core count (`lscpu`).

**No third-party networking or HTTP libraries.** The point of the project is to implement the networking yourself on top of POSIX APIs. Only use libraries for testing and benchmarking.

---

## 3. Scope

### In scope
- HTTP/1.1 forward proxy for plain `http://` requests (absolute-URI form, e.g. `GET http://example.com/ HTTP/1.1`)
- HTTPS via **`CONNECT host:443`** tunnelling
- IPv4 and IPv6
- Zero Trust destination policy, SNI verification, per-client rate limiting, proxy authentication
- Metrics, structured logs, config reload, graceful shutdown

### Out of scope (deliberate, see §14)
- **No TLS interception / decryption**
- No HTTP/2 or HTTP/3 (QUIC)
- No response caching
- No OS-wide traffic capture (clients must be configured to use the proxy)

---

## 4. Architecture

```
            clients (curl, browser, SentinelAgent)
                        │  TCP
     ┌──────────────────▼──────────────────────────────────────────────┐
     │ zt-proxy process                                                 │
     │                                                                  │
     │  worker 0 ─ epoll loop ─┐   SO_REUSEPORT: the kernel spreads new │
     │  worker 1 ─ epoll loop ─┼─  connections across N listening       │
     │  worker N ─ epoll loop ─┘   sockets (one per worker thread)      │
     │        │                                                         │
     │        ├─ Connection state machine (per client)                  │
     │        │    READ_REQUEST → AUTH → POLICY_PRECHECK → RESOLVING     │
     │        │    → POLICY_IP_CHECK → CONNECTING_UPSTREAM               │
     │        │    → (CONNECT) SEND_200 → READ_CLIENTHELLO → SNI_CHECK   │
     │        │    → RELAYING ↔ (backpressure) → CLOSING                 │
     │        │                                                         │
     │        └─ DNS: blocking getaddrinfo on a resolver thread pool;   │
     │             the result returns via eventfd to the worker's loop  │
     │                                                                  │
     │  admin listener (127.0.0.1:9100): GET /metrics, GET /healthz      │
     │  access log: async JSONL writer thread                            │
     └──────────────────┬──────────────────────────────────────────────┘
                        │  TCP to the pinned IP only
                     internet
```

### Key design decisions (you must be able to justify each)
1. **Event loop per thread with `epoll`, not thread-per-connection.** Thread-per-connection doesn't scale to thousands of idle connections (memory per stack, context switches). This is the classic C10k problem.
2. **`SO_REUSEPORT`** with one listener per worker: the kernel balances accepts, so workers share no state.
3. **Edge-triggered `epoll` (`EPOLLET`)** with "read until `EAGAIN`" loops. Document why, and the starvation risk you guard against (a per-event read budget).
4. **Non-blocking `connect()`** (`EINPROGRESS` → wait for `EPOLLOUT` → check `SO_ERROR`).
5. **DNS off the event loop:** `getaddrinfo` blocks, so it runs on a small thread pool and posts results back through an `eventfd`. The event loop must never block.
6. **IP pinning:** the proxy connects to the exact address that passed the policy check (prevents DNS rebinding).
7. **Backpressure:** if the destination socket can't take more data (`EAGAIN` on write), stop reading from the source (drop `EPOLLIN`) and wait for `EPOLLOUT`. Fixed-size per-direction buffers, so memory use per connection is bounded.
8. **No decryption:** HTTPS policy uses `CONNECT` host + port + SNI + resolved IP. Stated limitation: Encrypted ClientHello (ECH) would hide SNI.

---

## 5. Source layout

```
zt-proxy/
├── CMakeLists.txt            presets: debug, release, asan, tsan, fuzz
├── CMakePresets.json
├── config/zt-proxy.json      example configuration
├── src/
│   ├── main.cpp              argument parsing, signals, thread start/stop
│   ├── net/
│   │   ├── socket.{h,cpp}    RAII fd wrapper, non-blocking helpers, setsockopt
│   │   ├── event_loop.{h,cpp} epoll wrapper, timers (timerfd), eventfd wakeups
│   │   └── addr.{h,cpp}      sockaddr helpers, IPv4/IPv6 classification
│   ├── http/
│   │   └── request_parser.{h,cpp}  incremental HTTP/1.1 request-line + header parser
│   ├── tls/
│   │   └── sni.{h,cpp}       TLS ClientHello → SNI extractor (pure function)
│   ├── policy/
│   │   ├── policy.{h,cpp}    rule engine, decisions with rule id + reason
│   │   ├── host.{h,cpp}      hostname validation, punycode / IDN detection
│   │   └── rate_limit.{h,cpp} token bucket per client IP
│   ├── dns/
│   │   └── resolver.{h,cpp}  thread pool + result queue + eventfd notification
│   ├── proxy/
│   │   ├── connection.{h,cpp} per-connection state machine
│   │   ├── relay.{h,cpp}     bidirectional relay with backpressure
│   │   └── worker.{h,cpp}    one thread = one event loop + listener
│   ├── admin/
│   │   └── metrics.{h,cpp}   atomic counters, latency histogram, /metrics endpoint
│   ├── log/
│   │   └── access_log.{h,cpp} lock-free or mutex queue + writer thread, JSONL
│   └── config/
│       └── config.{h,cpp}    JSON config load + validation, hot reload on SIGHUP
├── tests/
│   ├── unit/                 GoogleTest: parser, SNI, policy, addresses, rate limiter
│   ├── integration/          pytest: real proxy, real sockets, curl/openssl
│   └── fuzz/                 libFuzzer targets: HTTP parser, SNI parser
├── bench/                    wrk scripts, local backend server, results/
├── deploy/                   Dockerfile, systemd unit
├── .github/workflows/ci.yml  build + tests + sanitizers on ubuntu-latest
└── README.md
```

**C++ rules:** C++20; RAII for every file descriptor and allocation; `std::unique_ptr`, no raw `new`/`delete`; `std::span`/`std::string_view` for parsing without copies; bounds-checked parsing (never trust lengths from the network); `-Wall -Wextra -Werror` in CI; `clang-tidy` and `clang-format` configs checked in.

---

## 6. Component requirements

### 6.1 Listener and event loop
- Bind to a configurable address (default `127.0.0.1:3128`). Binding to a non-loopback address must be an explicit config choice and must require authentication (§6.6).
- One worker thread per core (configurable), each with its own `SO_REUSEPORT` listener, `epoll` instance, `timerfd` for timeouts, and `eventfd` for cross-thread wake-ups.
- Ignore `SIGPIPE`; use `MSG_NOSIGNAL` / handle `EPIPE`.
- Limits: maximum connections per worker, maximum connections per client IP; when exceeded, accept and immediately close with a logged reason.

### 6.2 HTTP request parser (`http/request_parser`)
- **Incremental:** accept bytes in arbitrary chunks (the request may arrive in pieces).
- Parse the request line (`METHOD target HTTP/1.1`) and headers until `\r\n\r\n`.
- **Hard limits:** request line ≤ 8 KB, total headers ≤ 16 KB, ≤ 100 headers. Exceeding them → `431` / `400` and close.
- Accept `CONNECT host:port` and absolute-URI `GET/POST/... http://host[:port]/path`. Reject origin-form requests (`GET /path`) as not-a-proxy-request (`400`).
- Handle `Content-Length` and **`Transfer-Encoding: chunked`** request bodies for forwarding. **Reject requests containing both** headers. That's the classic HTTP request-smuggling vector; explain it in the README.
- Strip hop-by-hop headers (`Proxy-Authorization`, `Proxy-Connection`, `Connection`-listed headers) before forwarding.

### 6.3 TLS SNI extractor (`tls/sni`)
- A pure function: bytes in → `{status, hostname}` out. Statuses: `ok`, `no_sni`, `need_more_data`, `not_tls`, `malformed`.
- Walk the TLS record header → Handshake header → ClientHello → skip version, random, session id, cipher suites, compression → extensions → `server_name` (type 0). **Check every length against the remaining buffer** before reading.
- Must handle a ClientHello split across multiple TCP reads (buffer up to a limit such as 16 KB, then give up).
- This parser receives attacker-controlled bytes, so it is a **fuzzing target** (§8.3).

### 6.4 Destination policy (`policy/`)
Evaluated in order; the first match wins; every decision carries a **rule id** and **reason**, which are logged and returned in a header (`X-ZT-Block: <rule>`).

| # | Rule | Default action |
|---|---|---|
| 1 | Client failed authentication | `407` (`auth`) |
| 2 | Client exceeded its rate limit | `429` (`rate-limit`) |
| 3 | Destination port not allowed (default allow 443; 80 only for explicitly allowlisted hosts) | block (`port`) |
| 4 | Host is a raw IP literal and not explicitly allowlisted | block (`ip-literal`) |
| 5 | Host fails syntax checks, or is punycode (`xn--`) / mixed-script and not explicitly allowlisted | block (`suspicious-host`) |
| 6 | Host is on the **deny list** | block (`denylisted`) |
| 7 | Host is **not on the allowlist** (exact or `*.suffix` wildcard match) | block (`not-allowlisted`). Deny by default |
| 8 | **Resolve DNS:** any resolved address is private (RFC 1918), loopback, link-local (incl. `169.254.169.254` cloud metadata), CGNAT `100.64.0.0/10`, multicast, reserved, unspecified, or an IPv6 equivalent (ULA `fc00::/7`, link-local `fe80::/10`, IPv4-mapped forms) | block (`private-ip`) |
| 9 | `CONNECT` tunnel: **SNI present and ≠ `CONNECT` host** | block (`sni-mismatch`). Domain-fronting protection |
| 10 | `CONNECT` tunnel: first bytes are **not TLS** | block (`not-tls`). Stops raw-TCP tunnels through the proxy |
| 11 | Plain HTTP to a non-allowlisted-for-HTTP host | block (`tls-required`) |
| 12 | Otherwise | **allow**, connect to the **pinned IP** |

Configuration (`config/zt-proxy.json`) holds the allowlist, deny list, allowed ports, rate limits, timeouts, worker count, auth tokens and admin address. **`SIGHUP` reloads the policy atomically** (build a new immutable policy object, then swap a `std::shared_ptr` with `std::atomic` operations), so in-flight connections keep the old policy and new ones use the new one.

### 6.5 Rate limiting (`policy/rate_limit`)
- Token bucket per client IP: requests per second plus a burst size, configurable.
- The bucket state is per worker (no cross-thread locking). Document that this makes the effective limit per worker, or justify a shared sharded map if you implement a global limit.

### 6.6 Authentication
- `Proxy-Authorization: Bearer <token>` (or Basic), compared in **constant time** against tokens in the config (`CRYPTO_memcmp`-style loop; explain why a normal string compare leaks timing).
- Required whenever the listener isn't loopback-only. Optional on loopback (configurable).

### 6.7 Relay (`proxy/relay`)
- Two directions, each with a fixed-size buffer (e.g. 64 KB).
- Read until `EAGAIN` (edge-triggered) with a per-event budget; write as much as possible; **on partial write, keep the remainder and switch the source's interest off** (backpressure).
- Half-close handling: when one side sends FIN, `shutdown(SHUT_WR)` the other side after flushing, and finish when both directions are closed.
- Timeouts: connect timeout, idle timeout, and a maximum tunnel lifetime (all configurable, enforced with `timerfd`).
- Count bytes per direction for logs and metrics.
- **Stretch:** a zero-copy path using `splice()` through a pipe. Benchmark it against the buffered path and report the difference honestly, even if it's small.

### 6.8 DNS resolver (`dns/resolver`)
- A fixed-size thread pool running `getaddrinfo` with a timeout guard.
- Results go into a queue and wake the owning worker via its `eventfd`. The worker must ignore results for connections that have since closed (use a generation id).
- Optional: a small TTL-respecting cache. If you add it, test that a changed record can't bypass the private-IP check.

### 6.9 Metrics and admin (`admin/metrics`)
- Admin listener on `127.0.0.1:9100`:
  - `GET /healthz` → `200 ok`
  - `GET /metrics` → **Prometheus text format**: active connections, total connections, requests by method, decisions by rule id, bytes up/down, DNS latency, upstream connect latency, and a **request latency histogram**
- Counters are `std::atomic` with relaxed ordering, or per-worker counters summed on read. Explain the choice.

### 6.10 Access log (`log/access_log`)
- One JSONL record per request or tunnel: timestamp, client IP, method, host, port, SNI, resolved IP, decision, rule id, status code, bytes up/down, duration in ms.
- Written by a dedicated thread so disk I/O never blocks an event loop. Bounded queue; if it's full, drop records and count the drops (metric).

### 6.11 Process lifecycle
- `SIGINT`/`SIGTERM`: stop accepting, let existing connections drain up to a timeout, then exit cleanly (zero leaks under Valgrind).
- `SIGHUP`: reload config (§6.4).
- Run as an unprivileged user. Never require root (use a port above 1024).

---

## 7. Hardening the proxy itself
The proxy parses attacker-controlled bytes, so it must not become the vulnerability.

- **Sanitizer builds (CMake presets):** `asan` (AddressSanitizer + UndefinedBehaviorSanitizer) and `tsan` (ThreadSanitizer). The full test suite must pass clean under both.
- **Valgrind:** `valgrind --leak-check=full` over an integration run reports **0 leaks, 0 errors**.
- **Fuzzing (libFuzzer, clang):** fuzz targets for the HTTP request parser and the SNI parser, with a seed corpus of real requests and ClientHellos. Run each for a fixed amount of CPU time and report the executions and crashes found (and fixed).
- **Static analysis:** `clang-tidy` with a checked-in config; CI fails on new warnings.
- **systemd hardening (deploy/):** `NoNewPrivileges=yes`, `ProtectSystem=strict`, `ProtectHome=yes`, `PrivateTmp=yes`, `RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX`, `CapabilityBoundingSet=` (empty), `MemoryMax=` set.
- **Stretch:** a `seccomp-bpf` filter allowing only the system calls the proxy needs. List the allowed calls and test that a forbidden one kills the process.

---

## 8. Testing

### 8.1 Unit tests (GoogleTest)
- HTTP parser: valid requests, split at every byte position, oversize lines/headers, `Content-Length` + chunked conflict, malformed request lines, hop-by-hop header stripping.
- SNI parser: **recorded real ClientHello bytes** (capture them with `openssl s_client` + `tcpdump`), no-SNI, truncated at every offset, wrong lengths, non-TLS input.
- Address classification: every blocked IPv4 and IPv6 range, IPv4-mapped IPv6, boundaries of each range.
- Policy: every rule, its order, wildcard matching, punycode detection.
- Token bucket: refill, burst, time edge cases (use an injectable clock).
- Constant-time compare.

### 8.2 Integration tests (pytest, real sockets, local only)
Start the real proxy binary plus local backends: a plain HTTP server and a TLS server (`openssl s_server` or Python `ssl` with a test certificate). Use a **fake DNS mapping** (the config can map hostnames to IPs in test mode) so tests never touch the internet and always give the same result.

### 8.3 Red-team suite
A script (`tests/integration/redteam.py`) runs all scenarios and prints the **block rate** and **false-positive rate**. These are your resume numbers.

**Must be blocked:**
| # | Attack | Expected |
|---|---|---|
| R1 | `CONNECT` to a non-allowlisted host (exfiltration) | `not-allowlisted` |
| R2 | Host resolving to `127.0.0.1` / `10.0.0.5` / `192.168.1.1` (SSRF) | `private-ip` |
| R3 | `169.254.169.254` cloud-metadata access | `ip-literal` / `private-ip` |
| R4 | IPv6 tricks: `[::1]`, `[::ffff:127.0.0.1]`, ULA `fd00::1` | blocked |
| R5 | **DNS rebinding:** fake DNS returns a public IP first, then a private one | proxy connects only to the pinned public IP, or blocks |
| R6 | **Domain fronting:** `CONNECT allowed.example:443` but SNI = `evil.example` | `sni-mismatch` |
| R7 | **Raw TCP tunnel:** `CONNECT` then non-TLS bytes (e.g. SSH banner) | `not-tls` |
| R8 | Punycode look-alike of an allowlisted domain | `suspicious-host` |
| R9 | Disallowed ports (`:22`, `:25`, `:8443`) | `port` |
| R10 | Plain HTTP to a remote host | `tls-required` |
| R11 | No or wrong `Proxy-Authorization` (non-loopback mode) | `407` |
| R12 | Request flood from one client | `429` after the burst |
| R13 | **Request smuggling:** both `Content-Length` and `Transfer-Encoding` | `400` |
| R14 | Oversized headers / header bomb | `431`/`400`, proxy stays up |
| R15 | Slowloris: headers sent one byte per second | closed at the header timeout |
| R16 | Thousands of half-open connections | connection limits hold; other clients still served |

**Must be allowed (false-positive checks):**
| # | Legitimate case | Expected |
|---|---|---|
| L1 | `CONNECT` to an allowlisted host with matching SNI | tunnel works end to end (TLS handshake succeeds) |
| L2 | Wildcard-allowlisted subdomain | allowed |
| L3 | Large download through the tunnel (e.g. 500 MB) | byte-exact (verify SHA-256 on both ends) |
| L4 | ClientHello split across several TCP segments | SNI still extracted, allowed |
| L5 | Many concurrent legitimate clients | all served |

---

## 9. Benchmarks (`bench/`)
Run on your machine, against **local** backends, and report the hardware, kernel, compiler and flags. Every number on the resume comes from here.

| Benchmark | Tool | Report |
|---|---|---|
| HTTP request rate through the proxy vs direct to the backend | `wrk -t<N> -c<100,1000> -d30s` | requests/s, p50/p99 latency, proxy overhead |
| Scaling with worker threads: 1, 2, 4, N | `wrk` | requests/s per worker count (a scaling chart) |
| HTTPS tunnel throughput | `curl` large file via `CONNECT`, or `iperf3` through a tunnel | MB/s, CPU % |
| Concurrent idle connections | custom script opening many idle tunnels | memory per connection (RSS ÷ connections) |
| `splice()` vs buffered relay (stretch) | throughput test | MB/s difference |
| Profiling | `perf record` + flame graph | top hotspots, and one optimisation you made from it (before/after numbers) |

Put the raw outputs in `bench/results/` and a summary table in the README. **Explain anything surprising,** e.g. where scaling stops and why.

---

## 10. Observability and operations
- `/metrics` scraped live while `wrk` runs (include a sample output in the README).
- Show that `ss -tanp` matches the proxy's active-connection metric.
- Show the access log for each red-team case (blocked rule id visible).

---

## 11. Deployment and CI
- **Dockerfile** (multi-stage: build with the toolchain, run on a minimal image as a non-root user).
- **systemd unit** with the hardening options in §7.
- **GitHub Actions** (`ubuntu-latest`): configure + build (release), unit tests, integration tests, an ASan/UBSan job, and a TSan job. Badge in the README.

---

## 12. Integration with SentinelAgent
This replaces the **Python proxy** described in `sentinel_zero_trust_egress_spec.md` §4.1. The rest of that spec still applies.

- SentinelAgent's `EgressClient` sends all `httpx` traffic to `zt-proxy` (`proxy="http://127.0.0.1:3128"`, auth token, `trust_env=False`).
- **DLP stays in SentinelAgent** (Python, in-process). The proxy can't read HTTPS bodies, so secrets must be caught before encryption.
- The SentinelAgent red-team suite checks that blocked destinations surface as clear agent errors.
- **Running it on Windows:** SentinelAgent runs on Windows and zt-proxy on Linux. Either run zt-proxy in **WSL2 / Docker Desktop** on Windows and point SentinelAgent at it, or demonstrate the integration on Ubuntu. Document whichever you actually did.

---

## 13. Milestones
Work in order. Each milestone ends with passing tests and a commit. **Don't move on until you can explain the milestone's code without notes.**

| # | Milestone | Done when |
|---|---|---|
| M1 | Project skeleton: CMake presets, RAII socket wrapper, single-thread `epoll` echo server | Echo works with `nc`; ASan clean |
| M2 | Incremental HTTP parser + unit tests | All parser tests pass, including byte-by-byte splits |
| M3 | Plain HTTP forwarding (absolute-URI) with non-blocking connect | `curl -x` fetches a page from a local backend |
| M4 | `CONNECT` tunnelling + relay with backpressure + half-close | `curl -x` HTTPS to a local TLS server works; SHA-256 of a large file matches |
| M5 | Async DNS (resolver pool + `eventfd`) + destination policy + IP pinning | Policy unit tests and R1–R5, R8–R10 pass |
| M6 | SNI extractor + SNI-mismatch and not-TLS rules | Recorded ClientHello tests and R6, R7, L4 pass |
| M7 | Multithreading (`SO_REUSEPORT`, N workers) | TSan clean; scaling benchmark run |
| M8 | Auth, rate limiting, connection limits, timeouts | R11–R16 pass |
| M9 | Metrics endpoint, access log, config reload, graceful shutdown | `/metrics` live during `wrk`; `SIGHUP` reload test; Valgrind clean on shutdown |
| M10 | Fuzzing, `clang-tidy`, systemd hardening, Docker, CI | Fuzz runs reported; CI green |
| M11 | Benchmarks + profiling + one measured optimisation | `bench/results/` filled in; before/after numbers |
| M12 | SentinelAgent integration + README | Agent traffic visible in zt-proxy's access log |

---

## 14. Non-goals and known limitations (state them in the README)
- **No TLS interception.** Decisions use metadata only (host, SNI, IP, port). Enterprise proxies like Zscaler *can* inspect TLS by installing a trusted root certificate on managed devices. Explain that trade-off.
- **Encrypted ClientHello (ECH)** would hide SNI; the SNI check would then fall back to `CONNECT` host + IP policy.
- **Clients must opt in** by configuring the proxy. Transparent interception would need `iptables`/`nftables` `TPROXY` rules. That's a possible extension, not built.
- HTTP/1.1 only; no HTTP/2, QUIC or caching.

---

## 15. Honesty rules
- Every number (requests/s, latency, throughput, test counts, fuzz executions, block rate) must come from a real run and be reproducible from the scripts in the repo.
- If a scenario fails, fix it or list it as a known gap. Never quietly drop it.
- If AI tools helped write the code, that's fine, but **read and understand every module**. The interview will ask you to explain or whiteboard the event loop, the state machine and the SNI parser.

---

## 16. Interview preparation (must be able to explain)

### Networking
- TCP three-way handshake, FIN/RST, half-close, `TIME_WAIT` and why it exists
- What happens when you type a URL: DNS → TCP → TLS → HTTP
- HTTP/1.1 basics: request line, headers, `Content-Length` vs chunked, keep-alive
- Forward vs reverse proxy; what `CONNECT` does
- TLS handshake; why SNI is visible; what ECH changes
- DNS resolution; why `getaddrinfo` blocks
- RFC 1918 ranges, loopback, link-local, the cloud metadata address; what SSRF is
- DNS rebinding and why IP pinning stops it
- Domain fronting and why the SNI vs `CONNECT` check catches it
- HTTP request smuggling (`Content-Length` vs `Transfer-Encoding`)
- Nagle's algorithm / `TCP_NODELAY` (when it matters for a proxy)

### Linux / systems
- `select` vs `poll` vs `epoll`; level- vs edge-triggered and the "read until `EAGAIN`" rule
- Non-blocking `connect` and `SO_ERROR`
- `SO_REUSEADDR` vs `SO_REUSEPORT`
- `SIGPIPE` and why proxies ignore it
- `eventfd`, `timerfd`, file-descriptor limits (`ulimit -n`)
- Thread-per-connection vs event loop; the C10k problem
- `splice()` and zero-copy

### C++ / quality
- RAII, `unique_ptr` vs `shared_ptr`, move semantics, `string_view` lifetime pitfalls
- Data races vs race conditions; how TSan finds them; atomics and memory ordering basics
- What ASan, UBSan and Valgrind each catch
- What fuzzing is and why parsers are prime targets
- Constant-time comparison and timing attacks
- Token bucket algorithm

### Questions to rehearse
1. *"Walk me through, byte by byte, what happens when `curl -x` makes an HTTPS request through your proxy."*
2. *"Your proxy doesn't decrypt HTTPS. How do you decide what to allow, and what can't you see?"*
3. *"Why `epoll` and not one thread per connection? Where does your design stop scaling?"*
4. *"What happens when the client sends faster than the server can receive?"* (backpressure)
5. *"How would an attacker reach an internal service through your proxy, and how do you stop each way?"*
6. *"How did you find and fix a bug using GDB, a sanitizer or the fuzzer?"* (have a real story ready)
7. *"How is this related to what Zscaler does, and how would you scale it to millions of users?"*

---

## 17. Target resume bullets (placeholders; fill in measured values only)
- Built **zt-proxy**, an event-driven **C++20 HTTP/HTTPS forward proxy for Linux** (`epoll`, `SO_REUSEPORT`, non-blocking sockets) sustaining **X req/s** at **Y ms p99** across **N cores**.
- Enforced **Zero Trust egress**: domain allowlisting, **TLS SNI verification** without decryption, **DNS-pinned SSRF / private-IP blocking**, rate limiting; blocked **A/16 red-team attacks** with **0 false positives**.
- Hardened with **ASan/UBSan/TSan, Valgrind (0 leaks)** and **libFuzzer** (**Z M executions**) on the HTTP and TLS parsers; Prometheus metrics, systemd sandboxing and GitHub Actions CI.

Stack line: `{C++20, Linux, epoll, TCP/IP, TLS, CMake, GoogleTest, GDB, Valgrind, libFuzzer, Docker}`
