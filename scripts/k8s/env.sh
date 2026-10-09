#!/bin/bash
ZT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export PATH="$ZT_ROOT/artifacts/bin:$PATH"
export KUBECONFIG="$ZT_ROOT/artifacts/kubeconfig"
export KIND_EXPERIMENTAL_PROVIDER=docker
ZT_CLUSTER=zt-proxy
