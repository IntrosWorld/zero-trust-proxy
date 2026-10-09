#!/bin/bash
# Requires Docker access. Uses only the dedicated zt-proxy kind cluster.
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$ZT_ROOT"
if kind get clusters | grep -qx "$ZT_CLUSTER"; then
  echo 'Cluster zt-proxy already exists; use bench/k8s/run.py against it, or delete it deliberately first.' >&2
  exit 1
fi
cleanup() {
  kubectl get pods -A -o wide > bench/results/k8s/ci-pods.txt 2>&1 || true
  kind export logs artifacts/cluster-logs --name "$ZT_CLUSTER" || true
  kind delete cluster --name "$ZT_CLUSTER"
}
trap cleanup EXIT
# The baseline image is loaded by the cluster setup script.
docker build -f deploy/Dockerfile -t zt-proxy:local .
bash scripts/k8s/create-cluster.sh
bash scripts/k8s/build-images.sh
bash scripts/k8s/deploy.sh --set hpa.enabled=false
python3 bench/k8s/run.py E1 E2 E5 E8 E9 E10 --repeats "${ZT_ROLLOUT_REPEATS:-3}"
