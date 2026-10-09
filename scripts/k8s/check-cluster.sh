#!/bin/bash
# Requires Docker access. Refuses an existing cluster with the chosen name.
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$ZT_ROOT"
clusters=$(kind get clusters)
if grep -qx "$ZT_CLUSTER" <<< "$clusters"; then
  echo "Cluster $ZT_CLUSTER already exists; choose a separate test name or run individual experiments against it." >&2
  exit 1
fi
mkdir -p "$ZT_RESULTS_DIR"
cleanup() {
  kubectl get pods -A -o wide > "$ZT_RESULTS_DIR/ci-pods.txt" 2>&1 || true
  kind export logs "artifacts/cluster-logs-$ZT_CLUSTER" --name "$ZT_CLUSTER" || true
  kind delete cluster --name "$ZT_CLUSTER"
}
trap cleanup EXIT
# The baseline image is loaded by the cluster setup script.
docker build -f deploy/Dockerfile -t zt-proxy:local .
bash scripts/k8s/create-cluster.sh
bash scripts/k8s/build-images.sh
bash scripts/k8s/deploy.sh --set hpa.enabled=false
python3 bench/k8s/run.py E1 E2 E5 E8 E9 E10 --repeats "${ZT_ROLLOUT_REPEATS:-3}"
