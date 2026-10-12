#!/bin/sh
# Baixa só as animações de Black/White do repositório PokeAPI/sprites (~250 MB;
# o repositório inteiro tem 17 GB) para hd2d/external/pokeapi-sprites.
set -e
DEST="$(cd "$(dirname "$0")/.." && pwd)/external/pokeapi-sprites"
SUBDIR=sprites/pokemon/versions/generation-v/black-white/animated
if [ -d "$DEST/.git" ]; then
    git -C "$DEST" pull --depth 1
else
    git clone --filter=blob:none --no-checkout --depth 1 https://github.com/PokeAPI/sprites.git "$DEST"
    git -C "$DEST" sparse-checkout set --no-cone "$SUBDIR"
    git -C "$DEST" checkout
fi
echo "Sprites em $DEST/$SUBDIR"
