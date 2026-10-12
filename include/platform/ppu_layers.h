#ifndef GUARD_PLATFORM_PPU_LAYERS_H
#define GUARD_PLATFORM_PPU_LAYERS_H

#include <stdint.h>
#include "gba/defines.h"

// The software PPU can hand over a frame as separate layers instead of a
// finished image. The final GBA blend stage (priorities, windows, alpha
// and brightness) then runs on the GPU, where layers can be replaced by
// high resolution content (3D field, HD sprites).

enum
{
    PPU_LAYER_BG0,
    PPU_LAYER_BG1,
    PPU_LAYER_BG2,
    PPU_LAYER_BG3,
    PPU_LAYER_OBJ0, // sprites, one layer per priority
    PPU_LAYER_OBJ1,
    PPU_LAYER_OBJ2,
    PPU_LAYER_OBJ3,
    PPU_LAYER_COUNT
};

// Registers read by the blend stage, captured for every scanline because
// HBlank effects may change them mid-frame.
enum
{
    PPU_LINE_DISPCNT,
    PPU_LINE_BLDCNT,
    PPU_LINE_BLDALPHA,
    PPU_LINE_BLDY,
    PPU_LINE_BACKDROP, // palette color 0, before brightness effects
    PPU_LINE_BGPRIO,   // 2 bits per BG
    PPU_LINE_FLAGS,
    PPU_LINE_MOSAIC,
    PPU_LINE_BG2HOFS,  // map scroll: battle transitions change it per line
    PPU_LINE_BG2VOFS,
    PPU_LINE_BG2CNT,   // mosaic bit
    PPU_LINE_UNUSED,
    PPU_LINE_REG_COUNT // a multiple of 4 (uploaded as RGBA texels)
};

#define PPU_LINE_FLAG_WINDOWS (1 << 0) // a window is active on this line

struct PpuFrameLayers
{
    uint16_t layers[PPU_LAYER_COUNT][DISPLAY_HEIGHT][DISPLAY_WIDTH]; // BGR555, bit 15 set where drawn
    uint8_t winMask[DISPLAY_HEIGHT][DISPLAY_WIDTH];                  // per-pixel window enable bits
    uint8_t objIndex[4][DISPLAY_HEIGHT][DISPLAY_WIDTH];              // OAM index of each sprite layer pixel, 0xFF if none;
                                                                     // PPU_OBJ_RAW: color not alpha blended yet
    uint16_t lineRegs[DISPLAY_HEIGHT][PPU_LINE_REG_COUNT];
};

#define PPU_OAM_COUNT 128
#define PPU_OBJ_RAW   0x80 // objIndex flag (see PPU_DEFER_SPRITE_BLEND)

// DrawFrameEx flags
#define PPU_DEFER_SPRITE_BLEND 0x01 // leave the alpha blending of sprites to the GPU compositor: the
                                    // layers export them unblended and flagged PPU_OBJ_RAW (the 3D field
                                    // replaces the BG layers they would blend with)

// Renders one frame and runs the per-scanline interrupts and HBlank DMAs.
// Either output may be NULL: pixels receives the composed image, layers
// the unmerged layers. Sprites whose skipOam entry is set are left out
// (they are drawn by the GPU instead); skipOam may be NULL.
void DrawFrameEx(uint16_t *pixels, struct PpuFrameLayers *layers, const uint8_t *skipOam, int flags);

#endif // GUARD_PLATFORM_PPU_LAYERS_H
