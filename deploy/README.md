# Docker on Linux

This deployment was verified locally with Docker Engine 29.8.2: all 41 integration tests and the Compose lifecycle checks pass. The measured footprint is about 1.04 GB including Engine packages, images and build cache; the runtime image reports 126.5 MB. See [storage evidence](../bench/results/docker-storage.json).

Install [Docker Engine and the Compose plugin](https://docs.docker.com/engine/install/ubuntu/). Docker stores images and build cache on the Linux filesystem by default; copying this repository to another drive does not copy those images.

From the repository root:

```bash
sudo docker compose -f deploy/compose.yml up -d --build
sudo docker compose -f deploy/compose.yml ps
curl --fail http://127.0.0.1:9100/healthz
curl --fail http://127.0.0.1:9100/metrics
```

The proxy listens at `http://127.0.0.1:3128`. The supplied policy permits HTTPS to `example.com`, its subdomains and `api.openai.com`; normal upstream credentials are still required. For example:

```bash
curl --noproxy '' --proxy http://127.0.0.1:3128 https://example.com/
```

This deployment uses Linux host networking. Both listeners remain bound to the host's loopback interface. It is intended for clients on the same Linux machine. Containers do not force other processes to use the proxy. For remote clients or bridge networking, supply a configuration with a suitable listener address and required authentication, then configure the network exposure deliberately. The admin listener should remain private.

## Configuration and logs

Compose mounts `config/` read-only and reads `config/zt-proxy.json`. Edit the host file, validate it, then reload:

```bash
sudo docker compose -f deploy/compose.yml exec proxy \
  zt-proxy --config /etc/zt-proxy/zt-proxy.json --check-config
sudo docker compose -f deploy/compose.yml kill --signal SIGHUP proxy
sudo docker compose -f deploy/compose.yml logs --tail 30 proxy
```

Invalid reloads retain the last valid policy. Listener, worker and other startup settings require a restart; see the main README's reload rules. Atomic replacement of the config file works because the whole directory is mounted. Config files must be readable by UID/GID `10001:10001`; for token-bearing files use appropriate group ownership and `0640` permissions. Do not commit real tokens. Only the public sample config enters the image.

Startup/reload messages go to container stderr. Access logs live in the persistent `proxy-logs` volume:

```bash
sudo docker compose -f deploy/compose.yml exec proxy tail -n 20 /var/lib/zt-proxy/access.jsonl
sudo docker compose -f deploy/compose.yml exec proxy id
sudo docker compose -f deploy/compose.yml restart proxy
sudo docker compose -f deploy/compose.yml down
```

`down` preserves access logs. Adding `--volumes` deletes the Compose log volume. Access logs need an operational retention policy for long-running use.

The container runs as UID/GID 10001 with all Linux capabilities dropped, no new privileges, a read-only root filesystem, a 512 MiB memory ceiling and a 128-task limit. The writable log volume is separate. The default drain timeout is ten seconds; Compose allows 45 seconds before forced termination. Increase that grace period if you configure a longer drain timeout. Changing admin host/port also requires setting `ZT_ADMIN_HOST`/`ZT_ADMIN_PORT` in the container environment for the health check.

## Reproduce validation

Install host `python3-pytest`, `openssl` and `curl`, then run:

```bash
sudo python3 scripts/check_docker.py
# Reuse an already-built image:
sudo python3 scripts/check_docker.py --skip-build
# Requires free loopback ports 3128 and 9100:
sudo python3 scripts/check_compose.py
```

The script runs the full local HTTP/TLS/security integration suite against containers under the runtime restrictions above (256 MiB and 64 tasks per test container). Local test servers run on the host. Test-only DNS settings and writable fixtures stay in disposable directories. Each container carries a unique run label and is cleaned up after the run; production Compose containers are not touched. Reports go to `bench/results/docker*` and may be owned by root when invoked with sudo.

The Compose check verifies health, policy denial, SIGHUP reload, graceful shutdown, persistent logs and runtime restrictions. It uses a unique project name and removes only its own containers and log volume. Stop the regular proxy first if it occupies the default ports.

Inspect actual disk consumption:

```bash
sudo docker system df -v
sudo du -sh /var/lib/docker /var/lib/containerd
```

Image size, unpacked layers and build cache are different measurements and can share data. The compiler/build stage stays in cache, not the runtime image. Package downloads use HTTPS; an official Alpine image supplies the initial CA bundle until Ubuntu installs its own ca-certificates package. Rebuilding after cache removal downloads and compiles again. Do not prune shared Docker data indiscriminately.
