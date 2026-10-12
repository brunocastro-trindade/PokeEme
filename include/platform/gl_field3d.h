#ifndef GUARD_PLATFORM_GL_FIELD3D_H
#define GUARD_PLATFORM_GL_FIELD3D_H

#include <stdbool.h>
#include <stdint.h>
#include "platform/field3d.h"

// Draws the 3D field described by Field3D_Prepare into an offscreen color
// texture that the compositor uses in place of the map BG layers.
// Needs the GL functions loaded (GlRenderer_Init does it).

bool GlField3D_Init(void);
void GlField3D_Shutdown(void);

// Renders the scene at width x height; returns the color texture.
unsigned GlField3D_Render(const struct Field3DFrame *frame, int width, int height);

// Lighting, shadows, particles and post-processing follow the HD settings
// (hd_settings.h).

// The whole map from the game camera's angle, orthographic, at about
// pixelsPerBlock pixels per block (map editor previews). Returns RGB rows,
// top first, valid until the next call; NULL on failure.
const uint8_t *GlField3D_RenderOverview(const struct Field3DFrame *frame, int pixelsPerBlock, int *width, int *height);

#endif // GUARD_PLATFORM_GL_FIELD3D_H
