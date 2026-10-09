# ZT-Proxy — Distributed Deployment on Kubernetes: Implementation Plan

**Builds on:** zt-proxy (Linux copy `/home/mohit/projects/zt-proxy`, Windows copy `F:\College\Resume\zscaler\zt-proxy`)
**Cost:** free. Everything runs locally on Ubuntu. No cloud account or card needed.
**Goal:** Run zt-proxy as a **cluster of proxy nodes** managed by Kubernetes, with a **central policy control plane**, **cluster-wide rate limiting**, **monitoring**, **autoscaling**, **zero-downtime updates**, and **NetworkPolicy-enforced Zero Trust egress**. Measure each property with a repeatable experiment.

> **How to use this file:** copy it into `/home/mohit/projects/zt-proxy/docs/kubernetes-spec.md`, open Claude Code on Ubuntu in `/home/mohit/projects/zt-proxy`, and say:
> *"Implement `docs/kubernetes-spec.md` milestone by milestone. Read the existing code first. Stop after each milestone, show me the results, and explain what was built."*
> Work in the **Linux copy** (ext4), not the NTFS copy.

---

## 1. What changes

| | Today (single process) | After this plan (cluster) |
|---|---|---|
| Instances | 1 `zt-proxy` process | 3+ replicas behind one Kubernetes Service |
| Policy changes | Edit the JSON file, `SIGHUP` that process | Change once in the **policy control plane**; every replica pulls it, applies it by version, and reports its version |
| A crash | All traffic stops | Kubernetes restarts the pod; the other replicas keep serving |
| Load spikes | Fixed capacity | **Horizontal Pod Autoscaler** adds and removes replicas |
| Rate limiting | Per worker thread, inside one process | **Shared through Redis** across all replicas |
| Monitoring | `curl /metrics` by hand | **Prometheus + Grafana** dashboard for every replica |
| Upgrades | Stop, then start (short outage) | **Rolling update**, measured at zero failed requests |
| Other workloads | Must choose to use the proxy | **NetworkPolicy** makes zt-proxy their *only* way out to the internet |

### Why this matches Zscaler
Zscaler's cloud is built on the same principle: a central control plane manages policy and pushes it to many enforcement nodes in data centres worldwide. Nodes fail and recover without users noticing, and capacity scales with demand. This plan builds that architecture **in miniature, on one machine**. Describe it as "similar in design", never as a replica of their system.

---

## 2. Starting point (verified from the zt-proxy code)

| Existing feature | Where | How this plan uses it |
|---|---|---|
| JSON config with atomic swap (`ConfigStore`, `std::atomic_load`/`std::atomic_store` on `shared_ptr<const Config>`) | `src/config/config.h` | Policy updates from the control plane go through the same validated, atomic swap |
| `SIGHUP` reload that **rejects invalid configs** and keeps the old one | `src/main.cpp` | Same validation path for policies fetched over the network |
| `same_runtime()` check (some settings need a restart) | `src/config/config.h` | Decides which fields the control plane may change live (policy only) |
| `GET /healthz`, `GET /metrics` (Prometheus text) on the admin port | `src/admin/server.cpp` | Liveness probe + Prometheus scraping |
| `listen` defaults to `127.0.0.1`; `require_auth` and `tokens` exist | `src/config/config.h` | In the cluster it must listen on all interfaces, so **auth becomes mandatory** (tokens from a Kubernetes Secret) |
| Per-worker `RateLimiter` (`std::unordered_map` of token buckets) | `src/policy/rate_limit.h` | Kept as the **fallback** when Redis is unreachable |
| `deploy/Dockerfile`, `compose.yml`, `healthcheck.sh`, systemd unit | `deploy/` | The image is reused for Kubernetes |

**Missing today (added by this plan):** a readiness endpoint, policy versioning, pulling policy from a remote source, cluster-wide rate limiting, Kubernetes manifests, Helm chart and monitoring setup.

---

## 3. Tools (all free) — Ubuntu setup

```bash
# Docker Engine
sudo apt-get update
sudo apt-get install -y docker.io
sudo usermod -aG docker $USER   # log out and back in afterwards

# kind (Kubernetes in Docker), kubectl, Helm: install the official release binaries
# (check each project's install page for the current version; pin it and record it in the README)
#   kind:    https://kind.sigs.k8s.io/docs/user/quick-start/
#   kubectl: https://kubernetes.io/docs/tasks/tools/install-kubectl-linux/
#   Helm:    https://helm.sh/docs/intro/install/

# Load and benchmark tools (wrk is already used by zt-proxy's benchmarks)
sudo apt-get install -y wrk jq redis-tools
```

Record in the README: `docker version`, `kind version`, `kubectl version --client`, `helm version`, CPU model, cores and RAM.

**Machine sizing:** a 3-node kind cluster plus Prometheus/Grafana needs about **8 GB RAM** free. On less, use 1 control-plane + 1 worker node and say so.

---

## 4. Target architecture

```
                         kind cluster: 1 control-plane node + 2 worker nodes (Docker containers)
 ┌──────────────────────────────────────────────────────────────────────────────────────────────┐
 │ namespace: zt-system                                                                          │
 │                                                                                              │
 │   ┌────────────── Service: zt-proxy (ClusterIP :3128) ───────────────┐                       │
 │   │  zt-proxy pod 1    zt-proxy pod 2    zt-proxy pod 3   (… HPA)    │  data plane           │
 │   └───────┬──────────────────┬──────────────────┬────────────────────┘                       │
 │           │ GET /v1/policy   │ (poll / long-poll, ETag)                                      │
 │           ▼                  ▼                                                               │
 │   zt-policy (control plane): versioned policy store, rollout status, heartbeats              │
 │           ▲                                                                                  │
 │           │ PUT /v1/policy (admin token)                                                     │
 │   redis (cluster-wide token buckets, atomic Lua script)  ◄── every zt-proxy pod               │
 ├──────────────────────────────────────────────────────────────────────────────────────────────┤
 │ namespace: apps          sample client pods; NetworkPolicy: egress ONLY to kube-dns + zt-proxy│
 ├──────────────────────────────────────────────────────────────────────────────────────────────┤
 │ namespace: monitoring    Prometheus + Grafana (kube-prometheus-stack), ServiceMonitor scrapes │
 │                          each zt-proxy pod's /metrics                                          │
 └──────────────────────────────────────────────────────────────────────────────────────────────┘
        host (your Ubuntu): kubectl port-forward / NodePort for wrk, curl and the Grafana UI
```

**Network enforcement:** kind's default network plugin (kindnet) does **not** enforce NetworkPolicy. Create the cluster with the default CNI disabled and install **Calico** (free), or the NetworkPolicy milestone will look like it passes while enforcing nothing. Test it with a negative case (§7, E8).

---

## 5. Changes to zt-proxy itself (C++)

Keep the project's existing rules: C++20, RAII, no third-party networking/HTTP/JSON libraries, sanitizer-clean, tests for everything.

### 5.1 Readiness endpoint and graceful shutdown
- Add `GET /readyz` on the admin port. It returns `200` only when the listener is up **and** a valid policy is loaded, and `503` while starting up or draining.
  - **Liveness** = `/healthz` (is the process alive?). **Readiness** = `/readyz` (should it get traffic?). Be ready to explain the difference.
- On `SIGTERM`:
  1. switch `/readyz` to `503` so Kubernetes stops routing new connections
  2. keep serving for a short configurable delay (lets endpoint removal propagate)
  3. stop accepting new connections
  4. drain open connections up to a timeout
  5. exit 0
- Match this in the pod spec: `terminationGracePeriodSeconds` is longer than the drain timeout, plus an optional `preStop` sleep. Experiment E5 measures the difference this makes.

### 5.2 Policy versioning
- The config gains `policy_version` (integer) and a computed `policy_sha256`.
- New metric: `zt_policy_version` (gauge) and `zt_policy_reload_total{result="ok|rejected"}`.
- New admin endpoint: `GET /policy` → `{"version": N, "sha256": "...", "loaded_at": "..."}`.

### 5.3 Policy sync from the control plane
- New component `src/control/policy_sync.{h,cpp}`: a background thread (not on any event loop) that polls `GET /v1/policy` on the control plane.
  - **ETag / `If-None-Match`** → `304 Not Modified` when nothing changed (cheap polling).
  - Optional **long-poll** (`?wait=30`) for near-instant rollout.
  - **Jitter** on the poll interval, so all replicas don't hit the control plane at the same moment (thundering herd).
- On a new policy: **validate with the same code as `SIGHUP` reload**, then swap atomically through `ConfigStore`. An invalid policy is rejected, logged and counted, and **the last good policy stays active**.
- **Fail-static:** if the control plane is unreachable, keep enforcing the last good policy. Never fall back to "allow everything". Expose `zt_policy_sync_errors_total` and `zt_policy_age_seconds`.
- Only **policy fields** may change remotely (allowlist, denylist, ports, rate limits). Runtime fields (ports to listen on, worker count) still need a restart, using the existing `same_runtime()` logic.
- The control plane is reached over HTTP **inside the cluster**, authenticated with a token from a Kubernetes Secret. Document that production would use mutual TLS.

### 5.4 Cluster-wide rate limiting with Redis
- New component `src/policy/redis_limiter.{h,cpp}` plus a **minimal Redis client written from scratch**: speak the **RESP protocol** over a TCP socket. It's a good networking exercise and keeps the no-third-party rule.
- Token bucket stored in Redis and updated **atomically with a Lua script** (`EVALSHA`), keyed by client identity. Explain why a plain read-modify-write from several pods would race.
- Connection handling: a small connection pool, timeouts on every call, reconnect with backoff. **Never block an event loop on Redis.** Run calls on a helper thread or pool, the same way DNS is already handled.
- **Redis down:** configurable `fail_open` (fall back to the existing local per-worker limiter) or `fail_closed` (reject with `503`). Default: **fall back to local limits** and raise `zt_ratelimit_backend{backend="local"}`. Document the trade-off.
- Measure the latency Redis adds per request (p50/p99) and report it honestly. Optional optimisation: a small local token reservation per pod, which reduces round-trips at the cost of a little accuracy.

### 5.5 Logs
- Option to write the access log as JSON lines to **stdout**, so `kubectl logs` shows it and any cluster log collector can pick it up.

---

## 6. New components

### 6.1 Policy control plane (`control-plane/`, service name `zt-policy`)
A small service. **Python + FastAPI** is fine here (you're strongest in Python, and the data plane stays C++), or C++ if you prefer.

| Endpoint | Purpose |
|---|---|
| `GET /v1/policy` | Current policy + `version` + `sha256`; supports `ETag`/`If-None-Match` (`304`) and long-poll `?wait=` |
| `PUT /v1/policy` | Replace the policy (admin token required). **Validated** with the same rules as zt-proxy; increments the version; rejects invalid input with a clear error |
| `POST /v1/heartbeat` | Each zt-proxy pod reports `{pod, policy_version, ready}` |
| `GET /v1/rollout` | Which pods run which version, and how long the current rollout has taken |
| `GET /healthz`, `GET /metrics` | Health and Prometheus metrics |

- **Storage:** an SQLite file on a PersistentVolume (kind supports local volumes), or a ConfigMap. Keep a **version history** so you can roll back (`POST /v1/policy/rollback?to=N`).
- **Audit log:** every change records who, when, old version, new version and a diff summary.
- **Stretch: staged rollout.** Apply a new version to one pod first (canary), check its error rate, then to the rest.

### 6.2 Helm chart (`deploy/helm/zt-proxy/`)
Templates, with values for replica count, resources, tokens and policy-control-plane address:

| Object | Settings |
|---|---|
| **Deployment** zt-proxy | `replicas: 3`; requests/limits for CPU and memory; liveness `/healthz`; readiness `/readyz`; `RollingUpdate` with `maxUnavailable: 0`, `maxSurge: 1`; `topologySpreadConstraints` across worker nodes; `terminationGracePeriodSeconds` |
| **Pod security** | `runAsNonRoot`, `readOnlyRootFilesystem`, `allowPrivilegeEscalation: false`, drop **all** capabilities, `seccompProfile: RuntimeDefault` |
| **Service** zt-proxy | ClusterIP on 3128; admin port 9100 for scraping |
| **Secret** | Proxy auth tokens and control-plane token (never in Git; create them from a local file or with `kubectl create secret`) |
| **ConfigMap** | Bootstrap config (runtime settings); policy comes from the control plane |
| **PodDisruptionBudget** | `minAvailable: 2` |
| **HorizontalPodAutoscaler** | CPU target e.g. 60%, min 3, max 6 (requires metrics-server; install it in kind) |
| **Deployment + Service** zt-policy | 1 replica (state is in the volume); document that it's a single point of failure for *changes*, not for traffic, because proxies fail static |
| **Redis** | Single-instance Deployment or StatefulSet; document that it's not highly available |
| **NetworkPolicies** | See §6.3 |
| **ServiceMonitor** | For Prometheus to scrape every zt-proxy pod |

### 6.3 NetworkPolicies (Zero Trust inside the cluster)
| Namespace / pods | Ingress allowed | Egress allowed |
|---|---|---|
| `apps` (client workloads) | — | **Only** DNS (kube-dns, UDP/TCP 53) and TCP 3128 to the zt-proxy Service. **Everything else denied** |
| `zt-proxy` pods | From `apps` on 3128; from `monitoring` on 9100 | Internet (80/443), Redis, zt-policy, DNS |
| `zt-policy` | From zt-proxy pods; from an admin pod/port-forward | DNS only |
| `redis` | From zt-proxy pods only | — |

Start every namespace with **default-deny** and then allow the specific flows. That's Zero Trust applied to the cluster network.

### 6.4 Monitoring
- Install **kube-prometheus-stack** with Helm (Prometheus, Grafana, Alertmanager).
- Check in a **Grafana dashboard JSON** (`deploy/grafana/zt-proxy-dashboard.json`) with panels for:
  - requests/sec per pod
  - decisions by rule id (allowed vs each block reason)
  - p50/p99 latency
  - active connections
  - **policy version per pod** (shows rollouts live)
  - rate-limit rejections and the backend in use (Redis/local)
  - Redis and control-plane errors
- Optional `PrometheusRule` alerts: a zt-proxy pod down; **policy version skew** (pods on different versions for more than 60 s); Redis unreachable.

---

## 7. Experiments (each produces a resume-ready, measured number)
Automate each one as a script in `bench/k8s/` that prints its result, and save raw output in `bench/results/k8s/`.

| # | Experiment | Method | Report |
|---|---|---|---|
| **E1** | **Failover** | Steady `curl`/`wrk` load through the Service; delete one zt-proxy pod | Failed requests; time until the replacement pod is ready |
| **E2** | **Policy rollout latency** | `PUT` a new policy that blocks a test domain; a probe client checks until **every** pod blocks it (and `/v1/rollout` shows all on the new version). Repeat 20×, polling vs long-poll | p50/max rollout time to N pods |
| **E3** | **Horizontal scaling** | `wrk` throughput with 1, 2, 3, 6 replicas | req/s per replica count. **Note honestly** that all replicas share one machine's CPU, so scaling flattens |
| **E4** | **Autoscaling** | Sustained load; record replica count and CPU over time | A timeline: when the HPA scaled up and down |
| **E5** | **Zero-downtime rolling update** | Upgrade the image under load: once **without** readiness/drain handling, once **with** it | Failed requests in each case. Showing the difference is the point |
| **E6** | **Distributed rate limiting** | One client spreads requests across all pods; compare local-only vs Redis mode | Effective rate vs the configured limit |
| **E7** | **Redis outage** | Stop Redis during load | Behaviour (fallback to local limits), errors, recovery time |
| **E8** | **NetworkPolicy enforcement** | From an `apps` pod: direct `curl https://example.com` must **fail**; via zt-proxy it must **succeed** (if allowlisted) | Pass/fail for each path. A missing CNI shows up here |
| **E9** | **Control-plane outage** | Stop zt-policy | Proxies keep enforcing the last good policy (fail-static); sync-error metrics rise |
| **E10** | **Bad policy push** | `PUT` an invalid policy, plus one that's valid for the control plane but rejected by a proxy | Rejected without any pod losing its good policy; counters increment |

---

## 8. Tests and CI
- **Unit tests (C++):** policy-sync response parsing, ETag handling, version ordering; the RESP client (encode/decode, partial reads, errors); the Redis limiter's math with a fake backend; `/readyz` state transitions; the SIGTERM drain sequence.
- **Unit tests (control plane):** validation, versioning, rollback, ETag/304, long-poll timeout, auth.
- **Cluster tests:** a script that creates the kind cluster, installs the chart, waits for readiness, runs E1, E2, E5, E8, E9 and E10 as **pass/fail checks**, and tears down.
- **CI:** a GitHub Actions job that creates a kind cluster (free on public repos), installs the chart and runs the cluster checks. Keep the heavy benchmarks (E3, E4) local and document their results.
- All existing zt-proxy tests must keep passing, including sanitizer builds.

---

## 9. Milestones
| # | Milestone | Done when |
|---|---|---|
| K1 | Tools installed; kind cluster (with Calico); zt-proxy image built and loaded into kind | `kubectl get nodes` shows all nodes Ready |
| K2 | Plain manifests: Deployment (3 replicas), Service, Secret, probes using `/healthz` | `curl` through the Service works from inside and outside the cluster (port-forward) |
| K3 | `/readyz` + SIGTERM drain in zt-proxy | Unit tests pass; **E5** shows fewer failed requests with draining than without |
| K4 | Control plane service + policy versioning + policy sync in zt-proxy | **E2**, **E9** and **E10** pass; rollout status visible |
| K5 | Redis + RESP client + atomic Lua token bucket + fallback | **E6** and **E7** pass |
| K6 | Prometheus + Grafana + ServiceMonitor + dashboard JSON | Live dashboard during a `wrk` run (screenshot in the README) |
| K7 | HPA + metrics-server + PodDisruptionBudget + topology spread | **E3** and **E4** recorded |
| K8 | NetworkPolicies (default deny) | **E8** passes, including the negative case |
| K9 | Helm chart replaces the plain manifests; pod security settings | One command installs everything; security settings verified with `kubectl get pod -o yaml` |
| K10 | CI with kind, README section, experiment results, architecture diagram | CI green; every number in the README traceable to a script |

---

## 10. Honesty rules
- It's a **local multi-node kind cluster on one physical machine**. Say exactly that. Never "deployed across data centres" or "production".
- Scaling numbers are limited by one machine's CPU; report where they flatten and why.
- Every number comes from a script run in this project. Keep the raw output.
- If an experiment fails (e.g. some requests fail during a rolling update), report it and explain the cause. That makes a better interview story than a perfect-looking number.
- Redis and the control plane are single instances. State it as a known limitation.

---

## 11. Non-goals (say these in an interview if asked "what next?")
- Multi-cluster or multi-region deployment, global load balancing (anycast/GeoDNS)
- A service mesh (Istio/Linkerd) or mutual TLS between services (mention it as the production approach)
- Highly available Redis (Sentinel/Cluster) or a replicated control plane (Raft/etcd)
- Managed cloud Kubernetes (EKS/GKE/AKS). The same Helm chart could deploy there; free student credits make that a later option

---

## 12. Interview preparation (must be able to explain)

### Kubernetes basics
- Pod, ReplicaSet, Deployment, Service (ClusterIP / NodePort), Namespace
- **Liveness vs readiness vs startup probes**, and what goes wrong if you mix them up
- Rolling updates (`maxSurge`, `maxUnavailable`), and why readiness + draining gives zero downtime
- Resource requests vs limits; what an **OOMKill** is; CPU throttling
- ConfigMap vs Secret (and that Secrets are only base64-encoded unless encryption at rest is on)
- PodDisruptionBudget; topology spread
- Horizontal Pod Autoscaler and metrics-server
- NetworkPolicy, why it needs a CNI that enforces it, and **default-deny**
- The `SIGTERM` → grace period → `SIGKILL` shutdown sequence

### Distributed-systems ideas
- **Control plane vs data plane** and why they're separated
- **Eventual consistency** and versioning: pods briefly run different policy versions during a rollout. How you detect and bound that skew
- **Fail-static vs fail-open vs fail-closed:** what each means for the policy plane and the rate limiter, and which you chose and why
- **Atomicity across replicas:** why a Lua script in Redis instead of read-modify-write from each pod
- **Thundering herd** and jitter; ETag/304 and long-polling
- Single points of failure, and what proper high availability would need (replication, consensus)
- Trade-off: global rate-limit accuracy vs added latency per request

### Questions to rehearse
1. *"A pod is killed mid-request. What does the client see, and how did you minimise it?"*
2. *"You push a new policy. How long until every node enforces it, how do you know, and what happens to a node that can't reach the control plane?"*
3. *"Why do you need Redis for rate limiting? What happens when Redis goes down?"*
4. *"Your scaling graph flattens at N replicas. Why?"*
5. *"How do you make sure application pods can't bypass the proxy?"*
6. *"How would this design change for real data centres in several regions?"*

---

## 13. Resume wording (placeholders; fill in only measured values)
This **extends the zt-proxy entry**; it isn't a separate project. Merge the best of these into the zt-proxy bullets:

- Deployed zt-proxy as a **Kubernetes cluster service** (Helm, readiness-gated rolling updates, PDB, HPA) on a local multi-node kind cluster: **0 failed requests** across rolling upgrades and **X s** pod failover.
- Built a **versioned policy control plane** with ETag/long-poll sync and fail-static enforcement, rolling out policy to **N replicas in Y ms** (p50).
- Implemented **cluster-wide rate limiting** via a hand-written **Redis RESP client** and atomic Lua token buckets, with local fallback; enforced **default-deny NetworkPolicy** egress so workloads could only reach the internet through the proxy.

Stack additions: `{Kubernetes, Helm, kind, Calico, Redis, Prometheus, Grafana}`
