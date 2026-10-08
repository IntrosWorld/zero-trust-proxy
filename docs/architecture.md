# Architecture and security boundaries

## Request path

1. Linux distributes accepts among independent `SO_REUSEPORT` listeners. Every worker owns its connections, epoll instance, timerfd, token buckets and IP counts.
2. The incremental parser reads one bounded HTTP/1.1 request header. It rejects origin-form requests, duplicate framing headers, conflicting Content-Length/Transfer-Encoding, invalid header names, mismatched Host, folded headers and framing headers named by Connection.
3. Authentication runs before rate limiting and destination checks. The ordered destination checks are port, raw IP exemption, syntax/IDN, denylist, allowlist and HTTP exemption. The cheap HTTP exemption check runs before DNS instead of after SNI; a denied HTTP destination never causes resolution or connection.
4. A fixed resolver pool performs `getaddrinfo`, then posts checked-size results through a mutex queue and eventfd. Connection IDs increase monotonically; results for expired IDs are ignored. No DNS calls occur on a worker event loop.
5. Every DNS answer must be public. The connection retains the returned sockaddr array and connects directly to those values. A failed address can fall back only to another address from that same checked array. There is no second resolution during connection establishment.
6. A nonblocking connect finishes on EPOLLOUT after SO_ERROR is checked. CONNECT gets HTTP 200, then the proxy buffers up to 16 KiB of ClientHello records. No buffered TLS bytes reach the upstream until SNI validation passes. A different SNI, non-TLS first byte, malformed record or deadline closes the tunnel and logs the rule.
7. Two 64 KiB buffers relay traffic. EOF propagates as SHUT_WR only after pending bytes flush. HTTP request bodies are framed incrementally; after the one request finishes, upstream gets a write half-close. Response bytes stream back until upstream EOF.

## Why edge-triggered plus one-shot

Sockets use EPOLLET and EPOLLONESHOT. Reads and writes run until EAGAIN, buffer pressure, EOF, or a 256 KiB budget. The worker then computes interests and rearms using EPOLL_CTL_MOD. Rearming rechecks readiness, so a budget stop does not lose the only edge. This bounds one ready connection's work and avoids starvation. Listener accepts are level-triggered with a budget of 64; timerfd and eventfd are also level-triggered and drained.

On EAGAIN writing to a destination, the corresponding source loses EPOLLIN until the pending buffer drains. An inactive descriptor remains disarmed. The final implementation deliberately does not subscribe to RDHUP: EPOLLIN plus recv returning zero detects EOF, while persistent RDHUP on a half-closed, write-blocked fd could trigger a rearm busy loop.

## Isolation and shared state

Connection objects and rate buckets belong to one worker, requiring no locks. Per-client rate and connection limits are **per worker**; effective process allowance can be up to worker count times the configured limit. Use one worker for a strict single-bucket limit. The rate map is bounded at 65,536 clients, rejects new entries when full and prunes entries idle for ten minutes.

Immutable configs are exchanged with atomic shared_ptr load/store (the C++20 free-function API also works with GCC 11). Existing connections retain their snapshot. Shared counters use relaxed atomics because individual counters do not publish other memory. Bounded-cardinality rule and method maps use a small mutex. The log queue and each resolver mailbox have their own mutex; filesystem I/O happens only on the logger thread.

The bounded log queue holds 8,192 records; overflow and write errors increment `zt_log_drops_total`. The DNS job queue holds 4,096 jobs. Accept overload closes and records the reason. Admin connections are capped at 128, headers at 4 KiB, and their total lifetime at three seconds.

## HTTP framing and compatibility

Plain HTTP deliberately handles **one request per TCP connection**, rewrites absolute form to origin form, strips proxy/hop-by-hop headers and sends `Connection: close`. Fixed-length bodies are counted by block; chunked bodies validate size lines, delimiters and trailers while streaming. Ambiguous or coalesced surplus requests are rejected; later pipelined bytes are never forwarded. This avoids reusing a policy decision for a different HTTP destination.

A Content-Length plus Transfer-Encoding request can make two parsers disagree about where the request ends, allowing hidden requests to be interpreted downstream. Rejecting both together, duplicate framing and Connection-based framing deletion removes these ambiguity paths. Hop-by-hop removal preserves chunked framing because the outbound connection still uses Transfer-Encoding: chunked. Upgrade/WebSocket, Expect: 100-continue, absolute HTTPS URIs, folded headers and HTTP/1.0 client requests are rejected.

SNI extraction handles TCP fragmentation **and** handshake fragmentation across TLS records. It checks nested record, handshake, cipher, compression, extension and server-name lengths. The captured OpenSSL ClientHello is tested truncated at every byte. No-SNI/ECH clients fall back to CONNECT host and IP policy; they do not obtain an SNI match guarantee. HTTPS HTTP/2 can pass as opaque TLS bytes; the proxy itself only parses HTTP/1.1 and does not inspect protocols inside an established tunnel.

## Address and authentication policy

IPv4 checks reject unspecified, RFC1918, CGNAT, loopback, link-local, protocol assignment, documentation, benchmarking, multicast and reserved ranges. IPv6 permits global unicast 2000::/3 with additional rejection of IETF special assignments, documentation and 6to4; IPv4-mapped IPv6 gets the IPv4 checks. This conservative policy intentionally rejects transition mechanisms such as NAT64, rather than allowing embedded private targets.

DNS names must be ASCII DNS syntax. Operators use explicit punycode names for IDNs; wildcard grants alone never exempt suspicious IDNs or IP literals. `*.example.com` matches descendants, not the apex or `evilexample.com`. Deny rules override allowed names. An exact IP allowlist entry still does not exempt private address checks.

Bearer authentication compares every configured token using a loop over the longest input, accumulating differences without stopping at the first mismatch. Ordinary string comparison exposes matching-prefix information. Length is not treated as secret, and compiler/runtime/hardware behavior means this is not a formal side-channel proof. Header size bounds cap attacker-controlled work. Non-loopback listeners require auth; the admin listener must remain loopback.

## Timeouts and shutdown

A 100 ms timerfd tick enforces header/ClientHello, DNS, connect, idle and total lifetime limits. Header deadlines are absolute, so slow trickles do not renew them. DNS result timeout guards stop waiting connections and ignore late results. The operating system's blocking getaddrinfo cannot be safely force-cancelled by standard C++; a resolver already inside libc may delay the final resolver-thread join until the OS resolver returns. Event loops remain responsive. Configure sane system resolver retry/timeouts and use the service's stop timeout as the operational upper bound.

SIGINT/TERM stop accepting, allow connections to drain, then close leftovers at the configured deadline. SIGHUP parses and validates a replacement config before swapping it. Invalid reloads retain the old config. Listener addresses/ports, worker count, resolver count, log path and test mode require restart; other fields affect newly accepted connections. SIGPIPE is ignored and writes also use MSG_NOSIGNAL.

## Known boundaries

This is an explicit forward proxy, not an OS firewall. Clients must opt in; the Sentinel adapter disables environment overrides. There is no TLS interception, certificate inspection by the proxy, response caching, transparent routing, UDP/QUIC support, HTTP/2 proxy negotiation, splice path or seccomp filter. Clients must verify upstream certificates. Metadata policy cannot detect secrets inside TLS; SentinelAgent performs DLP before encryption.

The proxy listener itself is plaintext HTTP. Bearer credentials on a remote proxy connection need a trusted transport or a separate encrypted transport wrapper. The example binds loopback. The test-only DNS/private-host hooks require **both** `test_mode` and `--allow-test-config`, and test mode is forbidden on non-loopback listeners. Never use those hooks for real egress.

The access-log `status` is the **proxy decision/handshake status**, not a parsed backend response status; HTTPS backend statuses are encrypted. Duration is connection lifetime and bytes count wire traffic sent by the proxy, including protocol headers. Post-CONNECT rejection cannot send X-ZT-Block after HTTP 200; consult the structured log. No destination URLs, query strings, bodies or authentication tokens are logged.
