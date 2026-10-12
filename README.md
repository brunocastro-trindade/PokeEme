# PokeEme — Pokémon Emerald HD-2D

Port de Pokémon Emerald para **PC (Linux)** e **Nintendo Switch** (homebrew), com o mapa em 3D no estilo HD-2D.
O jogo em si é o mesmo do GBA: este projeto muda só a forma de desenhar o mundo.

Feito a partir do [pokeemerald](https://github.com/pret/pokeemerald) (decompilação do jogo) e do port
[pc_port](https://github.com/Kurausukun/pokeemerald/tree/pc_port), que roda o jogo nativamente com SDL2.

> Projeto de fã, sem fins lucrativos, sem ligação com a Nintendo, a Game Freak ou a The Pokémon Company.
> Este repositório não distribui ROMs nem o jogo compilado.

## O que tem

- **Mapa em 3D**: cidades, rotas, cavernas e interiores com alturas, paredes e telhados; personagens em pé
  no mundo; mapas vizinhos aparecem ao longe antes de você chegar neles.
- **Luz e sombras**, grama alta em pé que balança, desfoque de profundidade (tilt-shift), brilho e névoa.
- **Dia e noite**: a luz ao ar livre segue o relógio do jogo (amanhecer, dia, entardecer e noite).
- **Partículas** ao ar livre: pólen, folhas, brasas, bolhas, brilhos na água, chuva, passos.
- **Estilo Papel** (opcional): personagens viram recortes de papel, com borda branca, base e balanço ao andar.
- **Pokémon seguidor**, como em HeartGold/SoulSilver: o primeiro da equipe anda atrás de você.
- **Sprites animados** de Black/White nas batalhas.
- **Menu de efeitos** dentro do jogo para ligar e desligar cada coisa, com zoom da câmera.
- O save é o mesmo formato dos emuladores (`.sav` de 128 KB).

## Compilar

Precisa de Linux com `podman`, `git` e Python 3 com Pillow. As imagens de container (Fedora, devkitPro
para o Switch) são criadas sozinhas na primeira compilação.

```sh
# 1. dados do HD-2D (uma vez)
python3 hd2d/tools/mapconv/mapconv.py build                      # tabelas do mapa 3D
hd2d/scripts/fetch-follower-sprites.sh                           # sprites do seguidor (~16 MB)
python3 hd2d/tools/follower_convert.py hd2d/external/expansion hd2d/data/followers.bin
hd2d/scripts/fetch-pokeapi-sprites.sh                            # sprites de batalha (~260 MB)
python3 hd2d/tools/hdmon_convert.py \
    hd2d/external/pokeapi-sprites/sprites/pokemon/versions/generation-v/black-white/animated \
    hd2d/data/hdmon

# 2. PC
hd2d/scripts/build-linux.sh        # gera ./pokeemerald64
./pokeemerald64

# 3. Switch
hd2d/scripts/build-switch.sh       # gera ./pokeemerald.nro (com os dados dentro)
hd2d/scripts/package-switch.sh     # monta dist/switch/ para copiar para o cartão SD
```

Sem os dados do passo 1 o jogo roda normalmente, só que em 2D e sem os extras.

No Switch (com Atmosphère), abra o Homebrew Launcher segurando R ao iniciar um jogo. O save e as opções
ficam em `sdmc:/switch/pokeemerald/`.

## Controles

| GBA | Teclado | Controle |
|---|---|---|
| A / B | Z / X | A / B |
| Start / Select | Enter / `\` | + / − |
| L / R | A / S | L / R |
| Direcional | setas | direcional ou analógico |
| Acelerar | Espaço (segurar) | ZR |
| Menu de efeitos | Tab | ZL |

## Créditos

- [pret/pokeemerald](https://github.com/pret/pokeemerald) — decompilação do Pokémon Emerald
- [Kurausukun/pokeemerald `pc_port`](https://github.com/Kurausukun/pokeemerald/tree/pc_port) — port nativo com SDL2
- [PokeAPI/sprites](https://github.com/PokeAPI/sprites) — animações de Black/White
- [pokeemerald-expansion](https://github.com/rh-hideout/pokeemerald-expansion) — sprites do Pokémon seguidor
- Pokémon © Nintendo, Creatures Inc. e GAME FREAK inc.
