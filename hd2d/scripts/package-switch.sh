#!/bin/sh
# Monta dist/switch/ com o que vai para a raiz do cartão SD:
#   switch/pokeemerald/pokeemerald.nro   (já traz os dados do HD-2D dentro)
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$REPO/dist/switch/switch/pokeemerald"
rm -rf "$REPO/dist/switch"
mkdir -p "$OUT"
cp "$REPO/pokeemerald.nro" "$OUT/"
echo "Copie o conteúdo de $REPO/dist/switch/ para a raiz do cartão SD."
echo "Uma pasta data/ antiga em sdmc:/switch/pokeemerald/ pode ser apagada (os dados estão no .nro)."
