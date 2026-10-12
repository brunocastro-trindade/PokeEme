#!/bin/sh
# Baixa só os sprites de mapa ("overworld") do pokeemerald-expansion (~16 MB)
# para hd2d/external/expansion, sem o resto do repositório.
set -e
DEST="$(cd "$(dirname "$0")/.." && pwd)/external/expansion"
if [ -d "$DEST/.git" ]; then
    git -C "$DEST" pull --depth 1
else
    git clone --filter=blob:none --no-checkout --depth 1 https://github.com/rh-hideout/pokeemerald-expansion.git "$DEST"
    git -C "$DEST" sparse-checkout set --no-cone 'graphics/pokemon/**/overworld*'
    git -C "$DEST" checkout
fi
echo "Sprites em $DEST/graphics/pokemon"
