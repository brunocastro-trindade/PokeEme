#!/bin/sh
# Compila o .nro do Switch no container e deixa ./pokeemerald.nro na raiz do repositório.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
if ! podman image exists emerald-linux; then
    podman build -t emerald-linux -f "$HERE/../container/linux.Containerfile" "$HERE/../container"
fi
if ! podman image exists emerald-switch; then
    podman build -t emerald-switch -f "$HERE/../container/switch.Containerfile" "$HERE/../container"
fi
exec "$HERE/podman-run.sh" emerald-switch make switch -j"$(nproc)" "$@"
