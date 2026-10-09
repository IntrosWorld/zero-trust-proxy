#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/env.sh"
mkdir -p "$ZT_RESULTS_DIR"
clusters=$(kind get clusters)
if ! grep -qx "$ZT_CLUSTER" <<< "$clusters"; then
  kind create cluster --name "$ZT_CLUSTER" --config "$ZT_KIND_CONFIG" \
    --image kindest/node:v1.36.4@sha256:099e049362a1526b2db71494e1947aae99bd16290d7c895f2b7ea312e3cbfaed
fi
kubectl apply --server-side -f https://raw.githubusercontent.com/projectcalico/calico/v3.33.0/manifests/operator-crds.yaml
kubectl apply --server-side -f https://raw.githubusercontent.com/projectcalico/calico/v3.33.0/manifests/tigera-operator.yaml
kubectl apply -f "$ZT_ROOT/deploy/k8s/calico.yaml"
kubectl wait --for=condition=Ready nodes --all --timeout=600s
kind load docker-image zt-proxy:local --name "$ZT_CLUSTER"
{ docker version; kind version; kubectl version --client; helm version; lscpu; free -h; kubectl get nodes -o wide; } > "$ZT_RESULTS_DIR/environment.txt"
