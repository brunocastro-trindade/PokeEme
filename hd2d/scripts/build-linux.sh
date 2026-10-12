#!/bin/sh
# Compila o port de PC (Linux) no container e deixa ./pokeemerald64 na raiz do repositório.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
if ! podman image exists emerald-linux; then
    podman build -t emerald-linux -f "$HERE/../container/linux.Containerfile" "$HERE/../container"
fi
exec "$HERE/podman-run.sh" emerald-linux make linux -j"$(nproc)" "$@"
