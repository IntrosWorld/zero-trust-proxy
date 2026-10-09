#!/bin/bash
ZT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export PATH="$ZT_ROOT/artifacts/bin:$PATH"
export KIND_EXPERIMENTAL_PROVIDER=docker
export ZT_CLUSTER="${ZT_CLUSTER:-zt-proxy}"
if [ "$ZT_CLUSTER" = zt-proxy ]; then
  zt_default_kubeconfig="$ZT_ROOT/artifacts/kubeconfig"
  zt_default_results="$ZT_ROOT/bench/results/k8s"
else
  zt_default_kubeconfig="$ZT_ROOT/artifacts/kubeconfig-$ZT_CLUSTER"
  zt_default_results="$ZT_ROOT/bench/results/k8s/$ZT_CLUSTER"
fi
export KUBECONFIG="${ZT_KUBECONFIG:-$zt_default_kubeconfig}"
export ZT_KIND_CONFIG="${ZT_KIND_CONFIG:-$ZT_ROOT/deploy/k8s/kind.yaml}"
export ZT_RESULTS_DIR="${ZT_RESULTS_DIR:-$zt_default_results}"
