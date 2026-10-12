#ifndef GUARD_PLATFORM_FOLLOWER_GFX_H
#define GUARD_PLATFORM_FOLLOWER_GFX_H

#include <stdbool.h>
#include <stdint.h>

// Sprites of the following Pokémon, made by hd2d/tools/follower_convert.py.
// Keys: species numbers (include/constants/species.h), then Unown B..Z, !, ?
// at NUM_SPECIES + 1 + letter - 1 (the same numbers as SPECIES_UNOWN_B on).

#define FOLLOWER_GFX_FRAMES     9   // Emerald's standard walking layout
#define FOLLOWER_GFX_FRAME_SIZE 512 // 32x32, 4bpp

// Loads the file; without it there are no followers
bool FollowerGfx_Init(const char *path);

// Frames (FOLLOWER_GFX_FRAMES x FOLLOWER_GFX_FRAME_SIZE bytes) and the 16
// colors of a key, or NULL when there is no sprite for it
const uint8_t *FollowerGfx_Get(int key, bool shiny, const uint16_t **palette);

#endif // GUARD_PLATFORM_FOLLOWER_GFX_H
