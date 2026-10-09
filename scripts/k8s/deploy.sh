#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/env.sh"
cd "$ZT_ROOT"
python3 scripts/k8s/init-secrets.py
helm upgrade --install zt-proxy deploy/helm/zt-proxy --namespace zt-system --create-namespace --force-conflicts "$@" --wait --timeout 5m
kubectl apply -f deploy/k8s/clients.yaml
kubectl rollout status -n apps deployment/zt-client --timeout=180s
kubectl rollout status -n zt-system deployment/zt-admin --timeout=180s
