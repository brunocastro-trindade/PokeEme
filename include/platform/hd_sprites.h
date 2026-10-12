#ifndef GUARD_PLATFORM_HD_SPRITES_H
#define GUARD_PLATFORM_HD_SPRITES_H

#include <stdbool.h>
#include <stdint.h>

// HD Pokémon sprites: finds the 64x64 Pokémon pictures the game puts in OAM
// (by hashing their tiles against every front/back picture in the game) and
// describes how to draw an animated high resolution sprite in their place.
// The animations come from hd2d/tools/hdmon_convert.py.

#define HD_SPRITE_MAX_DRAWS 16

// One converted animation (a .hdm/.png pair)
struct HdAnim
{
    uint16_t width, height;   // frame size
    uint16_t columns;         // frames per atlas row
    uint16_t uniqueFrames;
    uint16_t sequenceLength;
    uint16_t paletteSize;
    bool hasShinyPalette;
    uint32_t palette[256];      // RGBA, R in the low byte
    uint32_t shinyPalette[256];
    uint16_t *frameIndex;       // atlas frame shown at each step
    uint16_t *durationMs;
    uint32_t totalDurationMs;
    uint8_t *atlas;             // palette indices
    int atlasWidth, atlasHeight;
    struct HdAnim *shinyAnim;   // separate shiny animation, when the shiny is not a palette swap
    unsigned gpuAtlas, gpuPalette; // GL textures, created by the renderer
};

struct HdSpriteDraw
{
    struct HdAnim *anim;
    int atlasFrame;
    bool shiny;               // use the shiny palette of anim
    float corners[4][2];      // screen position (GBA pixels) of the frame's TL, TR, BL, BR corners
    uint8_t oamIndex;
    uint8_t priority;
    bool semiTransparent;
    bool mosaic;
    float tint[4];            // palette fade: rgb (0..1) and amount (0..1)
};

// Builds the picture hash table. dataDir holds front/, back/, ... from the converter.
// Returns false (and HD sprites stay off) if the data directory is missing.
bool HdSprites_Init(const char *dataDir);
bool HdSprites_Enabled(void);
// Shows HD sprites (true) or the original pictures (false); needs Init to have succeeded.
void HdSprites_SetActive(bool active);
bool HdSprites_IsActive(void);

// Scans OAM for Pokémon pictures that have an HD animation. Fills draws (at
// most maxDraws) and sets skipOam[i] for each replaced OAM entry.
// timeMs drives the animations. Returns the number of draws.
int HdSprites_Collect(struct HdSpriteDraw *draws, int maxDraws, uint8_t *skipOam, uint32_t timeMs);

#endif // GUARD_PLATFORM_HD_SPRITES_H
