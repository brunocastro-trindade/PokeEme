#ifndef GUARD_PLATFORM_GL_RENDERER_H
#define GUARD_PLATFORM_GL_RENDERER_H

#include <stdbool.h>
#include <stdint.h>
#include "platform/field3d.h"
#include "platform/hd_sprites.h"
#include "platform/ppu_layers.h"

// GPU side of the video output: composes the PPU layers with the GBA blend
// rules, inserting HD sprites, and presents the result scaled to the window.

// Needs a current OpenGL ES 3.0 context. getProc loads GL functions.
bool GlRenderer_Init(void *(*getProc)(const char *name));
void GlRenderer_Shutdown(void);

// Composition happens at scale x the GBA resolution, so HD content keeps its
// detail. 1 gives the exact 240x160 image.
void GlRenderer_SetComposeScale(int scale);
int GlRenderer_ComposeScaleFor(int drawableWidth, int drawableHeight);

// Uploads a frame's layers and composes them, with the HD sprites and the
// 3D field (when field is active), into the frame image.
void GlRenderer_ComposeFrame(const struct PpuFrameLayers *layers, const struct HdSpriteDraw *draws, int drawCount,
                             const struct Field3DFrame *field);

// A 240x160 RGBA image laid over every composed frame (menus), or NULL.
// version tells when the pixels changed.
void GlRenderer_SetOverlay(const uint8_t *rgba, uint32_t version);

// Draws the last composed frame into the current framebuffer, letterboxed
// to the GBA aspect ratio. Sizes are in pixels.
void GlRenderer_Present(int drawableWidth, int drawableHeight);

// Reads the last composed frame back as BGR555 at 240x160 (top row first),
// taking the center of each scaled pixel.
void GlRenderer_ReadFrame(uint16_t *pixels);

// Reads the last composed frame at full compose resolution as RGB8 (top row
// first). Returns a buffer owned by the renderer; sets width and height.
const uint8_t *GlRenderer_ReadFrameRgb(int *width, int *height);

#endif // GUARD_PLATFORM_GL_RENDERER_H
