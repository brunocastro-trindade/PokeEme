#!/bin/sh
# Roda o jogo sem janela seguindo um roteiro de entrada e salva capturas.
# Uso: hd2d/scripts/run-headless.sh <roteiro> <quadros> <pasta_saida> [dump_every]
# Cria a pasta de saída e usa um save vazio próprio dentro dela.
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
INPUT="$(realpath "$1")"; FRAMES="$2"; OUT="$3"; EVERY="${4:-250}"
mkdir -p "$OUT"; OUT="$(realpath "$OUT")"
cd "$OUT"
rm -f pokeemerald.sav
PE_HEADLESS=1 PE_FRAMES="$FRAMES" PE_INPUT="$INPUT" PE_DUMP_EVERY="$EVERY" PE_DUMP_DIR="$OUT" \
    "${GAME:-$REPO/pokeemerald64}" > "$OUT/log.txt" 2>&1
