#!/bin/sh
# Roda um comando dentro de um container de build, com o repositório montado em /src.
# Uso: hd2d/scripts/podman-run.sh <imagem> <comando...>
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE="$1"; shift
exec podman run --rm --userns=keep-id -v "$REPO":/src:z -w /src "$IMAGE" "$@"
