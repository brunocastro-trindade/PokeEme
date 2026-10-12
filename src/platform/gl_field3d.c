#ifdef PORTABLE
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "platform/gl_api.h"
#include "platform/gl_field3d.h"
#include "platform/hd_settings.h"

// Camera: looks north, tilted down, with a narrow field of view so the
// picture stays close to the GBA framing (HD-2D style).
#define CAMERA_PITCH_DEG   40.0f
#define CAMERA_FOVY_DEG    30.0f
#define CAMERA_NEAR        1.0f
#define CAMERA_FAR         120.0f

// Sun shadows: a depth picture taken from the sun over a box around the
// camera target (blocks; Z grows south). It covers what the camera sees plus
// the things just outside that cast shadows into view.
#ifdef __SWITCH__
#define SHADOW_SIZE        1024
#else
#define SHADOW_SIZE        2048
#endif
#define SHADOW_BOX_X       16.0f
#define SHADOW_BOX_NORTH   15.0f
#define SHADOW_BOX_SOUTH   10.0f
#define SHADOW_BOX_TOP     7.0f

#define OVERVIEW_MARGIN    2.0f  // blocks of border around the map in overview pictures
#define OVERVIEW_TOP       4.0f  // height of the tallest things the overview must fit

// How each kind of map is lit. Lit ground gets ambient + sun, ground in
// shadow only ambient; upright things (cards, people) get "facade" of the sun
// whichever way they face, since their art already has its own shading.
struct LightSetup
{
    float sunDir[3];   // the way the light travels
    float sun[3];
    float ambient[3];
    float facade;
    float wind;        // how far grass tips sway (blocks)
    float dof;         // depth of field, 0 none, 1 full
    float bloom;
    float vignette;
    float fog[4];      // color and how much of it the far blurred part gets
    float saturation, contrast;
};

static const struct LightSetup sLightSetups[] = {
    [FIELD3D_LIGHT_OUTDOOR] = {
        .sunDir = {0.45f, -1.0f, 0.55f}, .sun = {0.46f, 0.42f, 0.31f}, .ambient = {0.56f, 0.60f, 0.72f},
        .facade = 0.8f, .wind = 0.07f, .dof = 0.85f, .bloom = 0.15f, .vignette = 0.22f,
        .fog = {0.82f, 0.88f, 0.96f, 0.16f}, .saturation = 1.08f, .contrast = 1.04f,
    },
    [FIELD3D_LIGHT_INDOOR] = {
        .sunDir = {0.25f, -1.0f, 0.30f}, .sun = {0.34f, 0.31f, 0.25f}, .ambient = {0.74f, 0.74f, 0.78f},
        .facade = 0.85f, .wind = 0.0f, .dof = 0.55f, .bloom = 0.12f, .vignette = 0.26f,
        .fog = {0.0f, 0.0f, 0.0f, 0.0f}, .saturation = 1.05f, .contrast = 1.03f,
    },
    [FIELD3D_LIGHT_CAVE] = {
        .sunDir = {0.30f, -1.0f, 0.40f}, .sun = {0.40f, 0.35f, 0.28f}, .ambient = {0.60f, 0.62f, 0.72f},
        .facade = 0.85f, .wind = 0.0f, .dof = 0.85f, .bloom = 0.18f, .vignette = 0.34f,
        .fog = {0.16f, 0.17f, 0.24f, 0.18f}, .saturation = 1.02f, .contrast = 1.06f,
    },
    [FIELD3D_LIGHT_UNDERWATER] = {
        .sunDir = {0.20f, -1.0f, 0.30f}, .sun = {0.30f, 0.38f, 0.42f}, .ambient = {0.62f, 0.70f, 0.82f},
        .facade = 0.85f, .wind = 0.05f, .dof = 1.0f, .bloom = 0.20f, .vignette = 0.30f,
        .fog = {0.30f, 0.50f, 0.78f, 0.28f}, .saturation = 1.0f, .contrast = 1.0f,
    },
};

// GLSL: every shader gets "#version 300 es" and its defines put in front.
// VRAM bytes are an R8UI texture 256 bytes wide; palettes an R16UI texture
// with 16 colors per row (rows 0-15 BG, 16-31 OBJ).
static const char sCommon[] =
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp usampler2D;\n"
    "precision highp sampler2DShadow;\n"
    "uniform usampler2D uPalette;\n"
    "uint vramByte(usampler2D tiles, uint index) {\n"
    "    return texelFetch(tiles, ivec2(int(index & 255u), int(index >> 8)), 0).r;\n"
    "}\n"
    "vec3 paletteColor(uint row, uint index) {\n"
    "    uint c = texelFetch(uPalette, ivec2(int(index), int(row)), 0).r;\n"
    "    vec3 v = vec3(float(c & 31u), float((c >> 5) & 31u), float((c >> 10) & 31u));\n"
    "    return (v * 8.0 + floor(v / 4.0)) / 255.0;\n"
    "}\n"
    "#ifndef SHADOW_PASS\n"
    "uniform sampler2DShadow uShadowMap;\n"
    "uniform float uShadowTexel;\n"
    "uniform vec3 uAmbient;\n"
    "uniform vec3 uSun;\n"
    "uniform float uFacade;\n"
    "uniform bool uShadowsOn;\n"
    "// 0 in shadow, 1 lit; 4 filtered taps for soft edges\n"
    "float sunlight(vec3 p) {\n"
    "    if (!uShadowsOn || p.x <= 0.0 || p.y <= 0.0 || p.x >= 1.0 || p.y >= 1.0 || p.z >= 1.0)\n"
    "        return 1.0;\n"
    "    float t = uShadowTexel * 0.75;\n"
    "    return 0.25 * (texture(uShadowMap, vec3(p.xy + vec2(-t, -t), p.z))\n"
    "                 + texture(uShadowMap, vec3(p.xy + vec2( t, -t), p.z))\n"
    "                 + texture(uShadowMap, vec3(p.xy + vec2(-t,  t), p.z))\n"
    "                 + texture(uShadowMap, vec3(p.xy + vec2( t,  t), p.z)));\n"
    "}\n"
    "vec3 light(vec3 lightPos, bool upright) {\n"
    "    return uAmbient + uSun * (upright ? uFacade : 1.0) * sunlight(lightPos);\n"
    "}\n"
    "#endif\n";

static const char sMapVS[] =
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec2 aUv;\n"
    "layout(location = 2) in uvec2 aMetatiles;\n"  // metatile, floor
    "layout(location = 3) in uvec4 aLayerFlags;\n" // layer, flags, sway, unused
    "layout(location = 4) in uint aCutMask;\n"
    "uniform mat4 uViewProj;\n"
    "uniform mat4 uLightViewProj;\n"
    "uniform vec3 uToSun;\n"   // a short step towards the sun (no self-shadowing)
    "uniform float uTime;\n"
    "uniform float uWind;\n"
    "uniform vec2 uActors[16];\n"
    "uniform int uActorCount;\n"
    "out vec2 vUv;\n"
    "out vec3 vLightPos;\n"
    "flat out uvec2 vMetatiles;\n"
    "flat out uvec2 vLayerFlags;\n"
    "flat out uint vCutMask;\n"
    "void main() {\n"
    "    vec3 p = aPos;\n"
    "    float sway = float(aLayerFlags.z) / 255.0;\n"
    "    if (sway > 0.0) {\n"
    "        // a gust that rolls across the field, and people parting the blades\n"
    "        float phase = uTime * 1.7 - p.x * 0.55 - p.z * 0.35;\n"
    "        float gust = sin(phase) * 0.65 + sin(phase * 2.3 + 1.3) * 0.25 + 0.35;\n"
    "        vec2 push = vec2(uWind * gust, uWind * 0.35 * gust);\n"
    "        float flatten = 0.0;\n"
    "        for (int i = 0; i < 16; i++) {\n"
    "            if (i >= uActorCount) break;\n"
    "            vec2 d = p.xz - uActors[i];\n"
    "            float dist = length(d);\n"
    "            float f = clamp(1.0 - dist / 1.15, 0.0, 1.0);\n"
    "            if (dist > 0.001) push += d / dist * f * 0.4;\n"
    "            flatten = max(flatten, f);\n"
    "        }\n"
    "        p.xz += push * sway;\n"
    "        p.y -= flatten * 0.3 * sway;\n"
    "    }\n"
    "    vUv = aUv;\n"
    "    vCutMask = aCutMask;\n"
    "    vMetatiles = aMetatiles;\n"
    "    vLayerFlags = aLayerFlags.xy;\n"
    "    vLightPos = (uLightViewProj * vec4(p + uToSun, 1.0)).xyz * 0.5 + 0.5;\n"
    "    gl_Position = uViewProj * vec4(p, 1.0);\n"
    "#ifdef SHADOW_PASS\n"
    "    if ((aLayerFlags.y & 8u) != 0u) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n" // no shadow: off screen
    "#endif\n"
    "}\n";

// A map pixel: metatile layer -> tile entry -> 4bpp pixel in BG VRAM -> BG palette
static const char sMapFS[] =
    "uniform usampler2D uTiles;\n"
    "uniform usampler2D uMetatiles;\n" // 8 entries per row, one row per metatile
    "in vec2 vUv;\n"
    "in vec3 vLightPos;\n"
    "flat in uvec2 vMetatiles;\n"
    "flat in uvec2 vLayerFlags;\n"
    "flat in uint vCutMask;\n"
    "#ifndef SHADOW_PASS\n"
    "out vec4 fragColor;\n"
    "#endif\n"
    "// returns color index (0 = transparent) and the tile's palette\n"
    "uvec2 metatilePixel(uint metatile, uint layer, ivec2 px) {\n"
    "    int slot = int(layer) * 4 + (px.y >> 3) * 2 + (px.x >> 3);\n"
    "    uint entry = texelFetch(uMetatiles, ivec2(slot, int(metatile)), 0).r;\n"
    "    ivec2 p = px & 7;\n"
    "    if ((entry & 0x400u) != 0u) p.x = 7 - p.x;\n"
    "    if ((entry & 0x800u) != 0u) p.y = 7 - p.y;\n"
    "    uint b = vramByte(uTiles, (entry & 0x3FFu) * 32u + uint(p.y) * 4u + uint(p.x) / 2u);\n"
    "    uint index = (p.x & 1) != 0 ? (b >> 4) : (b & 15u);\n"
    "    return uvec2(index, entry >> 12);\n"
    "}\n"
    "void main() {\n"
    "    vec2 uv = (vLayerFlags.y & 4u) != 0u ? mod(vUv, 16.0) : vUv;\n"
    "    ivec2 px = clamp(ivec2(floor(uv)), ivec2(0), ivec2(15));\n"
    "    uvec2 p = metatilePixel(vMetatiles.x, vLayerFlags.x, px);\n"
    "    if (p.x == 0u || ((vCutMask >> p.x) & 1u) != 0u)\n"
    "        discard;\n"
    "    vec3 color = paletteColor(p.y, p.x);\n"
    "    if (vCutMask == 0u && (vLayerFlags.y & 1u) != 0u) {\n" // cutout: same color as the floor here
    "        uvec2 f = metatilePixel(vMetatiles.y, 0u, px);\n"
    "        if (f.x != 0u && paletteColor(f.y, f.x) == color)\n"
    "            discard;\n"
    "    }\n"
    "#ifndef SHADOW_PASS\n"
    "    fragColor = vec4(color * light(vLightPos, (vLayerFlags.y & 2u) != 0u), 1.0);\n"
    "#endif\n"
    "}\n";

static const char sSpriteVS[] =
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec2 aLocal;\n"   // pixel inside the sprite
    "layout(location = 2) in uvec4 aSprite;\n" // tileNum, width, palette, flags
    "uniform mat4 uViewProj;\n"
    "uniform mat4 uLightViewProj;\n"
    "uniform vec3 uToSun;\n"
    "out vec2 vLocal;\n"
    "out vec3 vLightPos;\n"
    "flat out uvec4 vSprite;\n"
    "void main() {\n"
    "    vLocal = aLocal;\n"
    "    vSprite = aSprite;\n"
    "    vLightPos = (uLightViewProj * vec4(aPos + uToSun, 1.0)).xyz * 0.5 + 0.5;\n"
    "    gl_Position = uViewProj * vec4(aPos, 1.0);\n"
    "#ifdef SHADOW_PASS\n"
    "    if ((aSprite.w & 8u) != 0u) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);\n" // flat: casts no shadow
    "#endif\n"
    "}\n";

// A sprite pixel, addressed like the PPU does in 1D OBJ mapping. Paper
// figures get a white rim on the transparent pixels next to the figure (the
// quad was widened by a pixel on its outer sides); their stand is drawn here
// as a rounded card.
static const char sSpriteFS[] =
    "uniform usampler2D uObjTiles;\n"
    "uniform vec2 uFade;\n" // the game's palette fade: brightness, whiteness
    "in vec2 vLocal;\n"
    "in vec3 vLightPos;\n"
    "flat in uvec4 vSprite;\n"
    "#ifndef SHADOW_PASS\n"
    "out vec4 fragColor;\n"
    "#endif\n"
    "uint gWidth, gHeight, gFlags;\n"
    "// color index of a sprite pixel; 0: transparent, or outside the sprite\n"
    "uint indexAt(ivec2 p) {\n"
    "    if (p.x < 0 || p.y < 0 || p.x >= int(gWidth) || p.y >= int(gHeight)) return 0u;\n"
    "    if ((gFlags & 1u) != 0u) p.x = int(gWidth) - 1 - p.x;\n"
    "    if ((gFlags & 2u) != 0u) p.y = int(gHeight) - 1 - p.y;\n"
    "    uint block = uint(p.y / 8) * (gWidth / 8u) + uint(p.x / 8);\n"
    "    if ((gFlags & 4u) != 0u)\n"
    "        return vramByte(uObjTiles, (vSprite.x + block * 2u) * 32u + uint(p.y & 7) * 8u + uint(p.x & 7));\n"
    "    uint b = vramByte(uObjTiles, (vSprite.x + block) * 32u + uint(p.y & 7) * 4u + uint(p.x & 7) / 2u);\n"
    "    return (p.x & 1) != 0 ? (b >> 4) : (b & 15u);\n"
    "}\n"
    "vec3 paper(vec3 c) { return mix(c * uFade.x, vec3(1.0), uFade.y); }\n"
    "void main() {\n"
    "    gWidth = vSprite.y & 255u;\n"
    "    gHeight = vSprite.y >> 8;\n"
    "    gFlags = vSprite.w;\n"
    "    vec3 color;\n"
    "    if ((gFlags & 32u) != 0u) {\n" // the stand: an oval card with a darker edge
    "        vec2 halfSize = vec2(float(gWidth), float(gHeight)) * 0.5;\n"
    "        float r = length((vLocal - halfSize) / halfSize);\n"
    "        if (r > 1.0) discard;\n"
    "        color = paper(r > 1.0 - 1.2 / halfSize.y ? vec3(0.62, 0.60, 0.57) : vec3(0.90, 0.88, 0.84));\n"
    "    } else {\n"
    "        bool rim = (gFlags & 16u) != 0u;\n"
    "        ivec2 p = ivec2(floor(vLocal));\n"
    "        if (!rim) p = clamp(p, ivec2(0), ivec2(int(gWidth) - 1, int(gHeight) - 1));\n"
    "        uint index = indexAt(p);\n"
    "        if (index == 0u) {\n"
    "            bool touches = false;\n"
    "            if (rim)\n"
    "                for (int dy = -1; dy <= 1; dy++)\n"
    "                    for (int dx = -1; dx <= 1; dx++)\n"
    "                        touches = touches || indexAt(p + ivec2(dx, dy)) != 0u;\n"
    "            if (!touches) discard;\n"
    "            color = paper(vec3(0.97, 0.96, 0.92));\n"
    "        } else if ((gFlags & 4u) != 0u) {\n"
    "            color = paletteColor(16u + index / 16u, index & 15u);\n"
    "        } else {\n"
    "            color = paletteColor(16u + vSprite.z, index);\n"
    "        }\n"
    "    }\n"
    "#ifndef SHADOW_PASS\n"
    "    fragColor = vec4(color * light(vLightPos, (gFlags & 8u) == 0u), 1.0);\n"
    "#endif\n"
    "}\n";

// Particles: quads facing the camera (or lying flat), shapes drawn here.
// Output is premultiplied: glowing ones add light, the others cover.
static const char sParticleVS[] =
    "layout(location = 0) in vec3 aCenter;\n"
    "layout(location = 1) in vec2 aCorner;\n"    // -1..1
    "layout(location = 2) in vec2 aSizeAngle;\n"
    "layout(location = 3) in vec4 aColor;\n"
    "layout(location = 4) in uvec2 aShapeFlags;\n"
    "uniform mat4 uViewProj;\n"
    "uniform vec3 uRight;\n"
    "uniform vec3 uUp;\n"
    "out vec2 vLocal;\n"
    "out vec4 vColor;\n"
    "flat out uvec2 vShape;\n"
    "void main() {\n"
    "    float c = cos(aSizeAngle.y), s = sin(aSizeAngle.y);\n"
    "    vec2 r = vec2(aCorner.x * c - aCorner.y * s, aCorner.x * s + aCorner.y * c) * aSizeAngle.x;\n"
    "    vec3 p = (aShapeFlags.y & 1u) != 0u ? aCenter + vec3(r.x, 0.0, r.y) : aCenter + uRight * r.x + uUp * r.y;\n"
    "    vLocal = aCorner;\n"
    "    vColor = aColor;\n"
    "    vShape = aShapeFlags;\n"
    "    gl_Position = uViewProj * vec4(p, 1.0);\n"
    "}\n";

static const char sParticleFS[] =
    "precision highp float;\n"
    "in vec2 vLocal;\n"
    "in vec4 vColor;\n"
    "flat in uvec2 vShape;\n"
    "uniform vec2 uFade;\n"     // the game's palette fade: brightness, whiteness
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec2 q = vLocal;\n"
    "    float r = length(q), a;\n"
    "    vec3 rgb = vColor.rgb;\n"
    "    uint shape = vShape.x;\n"
    "    if (shape == 0u) {\n"                 // soft dot
    "        float f = clamp(1.0 - r, 0.0, 1.0);\n"
    "        a = f * f * 0.8 + smoothstep(0.55, 0.25, r) * 0.5;\n"
    "    } else if (shape == 1u) {\n"          // four-pointed star
    "        float core = exp(-r * r * 10.0);\n"
    "        float rays = max(clamp(1.0 - abs(q.y) * 7.0, 0.0, 1.0) * clamp(1.0 - abs(q.x), 0.0, 1.0),\n"
    "                         clamp(1.0 - abs(q.x) * 7.0, 0.0, 1.0) * clamp(1.0 - abs(q.y), 0.0, 1.0));\n"
    "        a = max(core, rays);\n"
    "    } else if (shape == 2u) {\n"          // leaf, pointed along x, a darker half
    "        float w = (1.0 - q.x * q.x) * 0.5;\n"
    "        a = step(abs(q.y), w);\n"
    "        rgb *= q.y > 0.0 ? 1.0 : 0.78;\n"
    "    } else if (shape == 3u) {\n"          // ring
    "        a = clamp(1.0 - abs(r - 0.78) * 5.5, 0.0, 1.0);\n"
    "    } else if (shape == 4u) {\n"          // bubble: rim, faint inside, a highlight
    "        a = clamp(1.0 - abs(r - 0.82) * 7.0, 0.0, 1.0) * 0.9 + step(r, 0.82) * 0.12\n"
    "          + clamp(1.0 - length(q - vec2(-0.3, 0.35)) * 5.0, 0.0, 1.0);\n"
    "    } else if (shape == 5u) {\n"          // puff
    "        float f = clamp(1.0 - r * r, 0.0, 1.0);\n"
    "        a = f * f;\n"
    "    } else {\n"                           // contact shadow: ellipse half as deep as wide
    "        a = smoothstep(1.0, 0.3, length(vec2(q.x, q.y * 2.0)));\n"
    "    }\n"
    "    a = clamp(a, 0.0, 1.0) * vColor.a;\n"
    "    if (shape == 6u) a *= uFade.x * (1.0 - uFade.y);\n"
    "    rgb = mix(rgb * uFade.x, vec3(1.0), uFade.y);\n"
    "    if (a < 0.004) discard;\n"
    "    fragColor = vec4(rgb * a, (vShape.y & 2u) != 0u ? 0.0 : a);\n"
    "}\n";

struct ParticleVertex
{
    float center[3];
    float corner[2];
    float sizeAngle[2];
    uint8_t color[4];
    uint8_t shape, flags, pad[2];
};

// Post-processing runs on fullscreen triangles; vUV covers [0,1]
static const char sPostVS[] =
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    vUV = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// Box filter: averages uTaps x uTaps bilinear taps (2x2 texels each)
static const char sDownsampleFS[] =
    "precision highp float;\n"
    "uniform sampler2D uSource;\n"
    "uniform vec2 uSourceTexel;\n"
    "uniform int uTaps;\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 c = vec3(0.0);\n"
    "    float center = float(uTaps - 1) * 0.5;\n"
    "    for (int y = 0; y < uTaps; y++)\n"
    "        for (int x = 0; x < uTaps; x++)\n"
    "            c += texture(uSource, vUV + (vec2(x, y) - center) * 2.0 * uSourceTexel).rgb;\n"
    "    fragColor = vec4(c / float(uTaps * uTaps), 1.0);\n"
    "}\n";

// 9-tap Gaussian along uStep, with 5 bilinear taps
static const char sBlurFS[] =
    "precision highp float;\n"
    "uniform sampler2D uSource;\n"
    "uniform vec2 uStep;\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec3 c = texture(uSource, vUV).rgb * 0.2270270;\n"
    "    c += (texture(uSource, vUV + uStep * 1.3846154).rgb + texture(uSource, vUV - uStep * 1.3846154).rgb) * 0.3162162;\n"
    "    c += (texture(uSource, vUV + uStep * 3.2307692).rgb + texture(uSource, vUV - uStep * 3.2307692).rgb) * 0.0702703;\n"
    "    fragColor = vec4(c, 1.0);\n"
    "}\n";

// Tilt-shift depth of field (sharp near the player, blurred towards the top
// and bottom of the screen), bloom, distance haze, grading and vignette
static const char sFinishFS[] =
    "precision highp float;\n"
    "uniform sampler2D uColor;\n"
    "uniform sampler2D uDepth;\n"
    "uniform sampler2D uBlur1;\n"
    "uniform sampler2D uBlur2;\n"
    "uniform vec2 uClip;\n"      // near, far planes
    "uniform float uFocus;\n"    // view distance in focus
    "uniform vec4 uDofRange;\n"  // near start, near length, far start, far length (blocks)
    "uniform float uDof;\n"
    "uniform float uBloom;\n"
    "uniform float uVignette;\n"
    "uniform vec4 uFog;\n"
    "uniform vec2 uGrade;\n"     // saturation, contrast
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "float viewDistance(float depth) {\n"
    "    float ndc = depth * 2.0 - 1.0;\n"
    "    return 2.0 * uClip.x * uClip.y / (uClip.y + uClip.x - ndc * (uClip.y - uClip.x));\n"
    "}\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    vec3 sharp = texelFetch(uColor, p, 0).rgb;\n"
    "    float d = viewDistance(texelFetch(uDepth, p, 0).r) - uFocus;\n"
    "    float nearBlur = clamp((-d - uDofRange.x) / uDofRange.y, 0.0, 1.0);\n"
    "    float farBlur = clamp((d - uDofRange.z) / uDofRange.w, 0.0, 1.0);\n"
    "    float coc = max(nearBlur * 0.6, farBlur) * uDof;\n"
    "    vec3 b1 = texture(uBlur1, vUV).rgb;\n"
    "    vec3 b2 = texture(uBlur2, vUV).rgb;\n"
    "    vec3 c = coc < 0.5 ? mix(sharp, b1, coc * 2.0) : mix(b1, b2, coc * 2.0 - 1.0);\n"
    "    float glow = max(dot(b2, vec3(0.299, 0.587, 0.114)) - 0.8, 0.0);\n"
    "    c += b2 * glow * uBloom * 3.0;\n"
    "    c = mix(c, uFog.rgb, uFog.a * farBlur);\n"
    "    float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
    "    c = mix(vec3(l), c, uGrade.x);\n"
    "    c = (c - 0.5) * uGrade.y + 0.5;\n"
    "    vec2 q = vUV - 0.5;\n"
    "    c *= 1.0 - uVignette * smoothstep(0.1, 0.9, dot(q, q) * 2.0);\n"
    "    fragColor = vec4(c, 1.0);\n"
    "}\n";

struct SpriteVertex
{
    float pos[3];
    float local[2];
    uint16_t sprite[4]; // tileNum, width | height << 8, palette, flags
};

// Uniform locations of the scene programs
struct SceneProgram
{
    GLuint program;
    GLint viewProj, lightViewProj, toSun, time, wind, actors, actorCount;
    GLint ambient, sun, facade, shadowTexel, shadowsOn, fade;
};

// An offscreen color target
struct Target
{
    GLuint texture, fbo;
    int width, height;
};

// A camera: matrices plus what the post-processing needs to know
struct View
{
    float viewProj[16];
    float lightViewProj[16];
    float toSun[3];
    float right[3], up[3]; // camera axes (particles face the camera)
};

static struct SceneProgram sMap, sMapShadow, sSprite, sSpriteShadow;
static GLuint sDownsampleProgram, sBlurProgram, sFinishProgram;
static GLint sDownsampleTexelLoc, sDownsampleTapsLoc, sBlurStepLoc;
static GLint sFinishClipLoc, sFinishFocusLoc, sFinishDofRangeLoc, sFinishDofLoc, sFinishBloomLoc;
static GLint sFinishVignetteLoc, sFinishFogLoc, sFinishGradeLoc;
static GLuint sMapVao, sMapVbo, sSpriteVao, sSpriteVbo, sPostVao;
static GLuint sBgTiles, sObjTiles, sPalette, sMetatileTexture;
static GLuint sShadowMap, sShadowFbo;
static GLuint sSceneFbo, sSceneColor, sSceneDepth;
static struct Target sDown1, sTemp1, sBlur1, sDown2, sTemp2, sBlur2, sFinal;
static int sWidth, sHeight;
static uint32_t sMeshVersion = (uint32_t)-1, sMetatileVersion = (uint32_t)-1, sPatchVersion;
static int sMapVertexCount;
static int sSpriteVertexCount;
static GLuint sParticleProgram, sParticleVao, sParticleVbo;
static GLint sParticleViewProjLoc, sParticleRightLoc, sParticleUpLoc, sParticleFadeLoc;
static uint8_t *sOverviewPixels;

static GLuint Compile(GLenum type, const char *defines, const char *common, const char *source)
{
    const char *parts[4] = {"#version 300 es\n", defines, common, source};
    GLuint shader = glCreateShader(type);
    GLint ok = GL_FALSE;

    glShaderSource(shader, 4, parts, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "field3d: shader compile failed:\n%s\n", log);
        return 0;
    }
    return shader;
}

static GLuint Link(const char *defines, const char *vsCommon, const char *vs, const char *fsCommon, const char *fs)
{
    GLuint v = Compile(GL_VERTEX_SHADER, defines, vsCommon, vs);
    GLuint f = Compile(GL_FRAGMENT_SHADER, defines, fsCommon, fs);
    GLuint program;
    GLint ok = GL_FALSE;

    if (v == 0 || f == 0)
        return 0;
    program = glCreateProgram();
    glAttachShader(program, v);
    glAttachShader(program, f);
    glLinkProgram(program);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "field3d: link failed:\n%s\n", log);
        return 0;
    }
    return program;
}

// Map and sprite programs, lit or for the shadow pass
static bool LinkScene(struct SceneProgram *out, const char *vs, const char *fs, bool shadowPass, bool sprite)
{
    const char *defines = shadowPass ? "#define SHADOW_PASS\n" : "";
    GLuint program = Link(defines, "precision highp float;\nprecision highp int;\n", vs, sCommon, fs);

    if (program == 0)
        return false;
    out->program = program;
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "uPalette"), 2);
    glUniform1i(glGetUniformLocation(program, sprite ? "uObjTiles" : "uTiles"), sprite ? 3 : 0);
    glUniform1i(glGetUniformLocation(program, "uMetatiles"), 1);
    glUniform1i(glGetUniformLocation(program, "uShadowMap"), 4);
    out->viewProj = glGetUniformLocation(program, "uViewProj");
    out->lightViewProj = glGetUniformLocation(program, "uLightViewProj");
    out->toSun = glGetUniformLocation(program, "uToSun");
    out->time = glGetUniformLocation(program, "uTime");
    out->wind = glGetUniformLocation(program, "uWind");
    out->actors = glGetUniformLocation(program, "uActors");
    out->actorCount = glGetUniformLocation(program, "uActorCount");
    out->ambient = glGetUniformLocation(program, "uAmbient");
    out->sun = glGetUniformLocation(program, "uSun");
    out->facade = glGetUniformLocation(program, "uFacade");
    out->shadowTexel = glGetUniformLocation(program, "uShadowTexel");
    out->shadowsOn = glGetUniformLocation(program, "uShadowsOn");
    out->fade = glGetUniformLocation(program, "uFade");
    return true;
}

static GLuint DataTexture(GLenum internalFormat, int width, int height, GLenum format, GLenum type)
{
    GLuint texture;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, NULL);
    return texture;
}

static GLuint DepthTexture(int width, int height, bool compare)
{
    GLuint texture;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, compare ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, compare ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (compare)
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    return texture;
}

// New textures are bound on a spare unit, so the ones the shaders read stay put
#define SPARE_TEXTURE_UNIT 7

static void MakeTarget(struct Target *target, int width, int height)
{
    glActiveTexture(GL_TEXTURE0 + SPARE_TEXTURE_UNIT);
    if (target->fbo != 0)
    {
        glDeleteFramebuffers(1, &target->fbo);
        glDeleteTextures(1, &target->texture);
    }
    glGenTextures(1, &target->texture);
    glBindTexture(GL_TEXTURE_2D, target->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &target->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target->texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "field3d: post-processing framebuffer incomplete\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    target->width = width;
    target->height = height;
}

static void FreeTarget(struct Target *target)
{
    glDeleteFramebuffers(1, &target->fbo);
    glDeleteTextures(1, &target->texture);
    target->fbo = target->texture = 0;
}

bool GlField3D_Init(void)
{
    if (!LinkScene(&sMap, sMapVS, sMapFS, false, false)
     || !LinkScene(&sMapShadow, sMapVS, sMapFS, true, false)
     || !LinkScene(&sSprite, sSpriteVS, sSpriteFS, false, true)
     || !LinkScene(&sSpriteShadow, sSpriteVS, sSpriteFS, true, true))
        return false;

    sParticleProgram = Link("", "precision highp float;\nprecision highp int;\n", sParticleVS, "", sParticleFS);
    if (sParticleProgram == 0)
        return false;
    sParticleViewProjLoc = glGetUniformLocation(sParticleProgram, "uViewProj");
    sParticleRightLoc = glGetUniformLocation(sParticleProgram, "uRight");
    sParticleUpLoc = glGetUniformLocation(sParticleProgram, "uUp");
    sParticleFadeLoc = glGetUniformLocation(sParticleProgram, "uFade");

    sDownsampleProgram = Link("", "", sPostVS, "", sDownsampleFS);
    sBlurProgram = Link("", "", sPostVS, "", sBlurFS);
    sFinishProgram = Link("", "", sPostVS, "", sFinishFS);
    if (sDownsampleProgram == 0 || sBlurProgram == 0 || sFinishProgram == 0)
        return false;
    glUseProgram(sDownsampleProgram);
    glUniform1i(glGetUniformLocation(sDownsampleProgram, "uSource"), 0);
    sDownsampleTexelLoc = glGetUniformLocation(sDownsampleProgram, "uSourceTexel");
    sDownsampleTapsLoc = glGetUniformLocation(sDownsampleProgram, "uTaps");
    glUseProgram(sBlurProgram);
    glUniform1i(glGetUniformLocation(sBlurProgram, "uSource"), 0);
    sBlurStepLoc = glGetUniformLocation(sBlurProgram, "uStep");
    glUseProgram(sFinishProgram);
    glUniform1i(glGetUniformLocation(sFinishProgram, "uColor"), 0);
    glUniform1i(glGetUniformLocation(sFinishProgram, "uDepth"), 1);
    glUniform1i(glGetUniformLocation(sFinishProgram, "uBlur1"), 2);
    glUniform1i(glGetUniformLocation(sFinishProgram, "uBlur2"), 3);
    sFinishClipLoc = glGetUniformLocation(sFinishProgram, "uClip");
    sFinishFocusLoc = glGetUniformLocation(sFinishProgram, "uFocus");
    sFinishDofRangeLoc = glGetUniformLocation(sFinishProgram, "uDofRange");
    sFinishDofLoc = glGetUniformLocation(sFinishProgram, "uDof");
    sFinishBloomLoc = glGetUniformLocation(sFinishProgram, "uBloom");
    sFinishVignetteLoc = glGetUniformLocation(sFinishProgram, "uVignette");
    sFinishFogLoc = glGetUniformLocation(sFinishProgram, "uFog");
    sFinishGradeLoc = glGetUniformLocation(sFinishProgram, "uGrade");

    sBgTiles = DataTexture(GL_R8UI, 256, 128, GL_RED_INTEGER, GL_UNSIGNED_BYTE);   // 1024 tiles
    sObjTiles = DataTexture(GL_R8UI, 256, 128, GL_RED_INTEGER, GL_UNSIGNED_BYTE);  // 32 KB OBJ VRAM
    sPalette = DataTexture(GL_R16UI, 16, 32, GL_RED_INTEGER, GL_UNSIGNED_SHORT);
    sMetatileTexture = DataTexture(GL_R16UI, 8, FIELD3D_METATILE_ROWS, GL_RED_INTEGER, GL_UNSIGNED_SHORT);

    sShadowMap = DepthTexture(SHADOW_SIZE, SHADOW_SIZE, true);
    glGenFramebuffers(1, &sShadowFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, sShadowFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sShadowMap, 0);
    {
        static const GLenum sNone[] = {GL_NONE};
        glDrawBuffers(1, sNone);
        glReadBuffer(GL_NONE);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "field3d: shadow framebuffer incomplete\n");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glGenVertexArrays(1, &sMapVao);
    glGenBuffers(1, &sMapVbo);
    glBindVertexArray(sMapVao);
    glBindBuffer(GL_ARRAY_BUFFER, sMapVbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct Field3DVertex), (void *)offsetof(struct Field3DVertex, pos));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct Field3DVertex), (void *)offsetof(struct Field3DVertex, uv));
    glVertexAttribIPointer(2, 2, GL_UNSIGNED_SHORT, sizeof(struct Field3DVertex), (void *)offsetof(struct Field3DVertex, metatile));
    glVertexAttribIPointer(3, 4, GL_UNSIGNED_BYTE, sizeof(struct Field3DVertex), (void *)offsetof(struct Field3DVertex, layer));
    glVertexAttribIPointer(4, 1, GL_UNSIGNED_SHORT, sizeof(struct Field3DVertex), (void *)offsetof(struct Field3DVertex, cutMask));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glEnableVertexAttribArray(4);

    glGenVertexArrays(1, &sSpriteVao);
    glGenBuffers(1, &sSpriteVbo);
    glBindVertexArray(sSpriteVao);
    glBindBuffer(GL_ARRAY_BUFFER, sSpriteVbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct SpriteVertex), (void *)offsetof(struct SpriteVertex, pos));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct SpriteVertex), (void *)offsetof(struct SpriteVertex, local));
    glVertexAttribIPointer(2, 4, GL_UNSIGNED_SHORT, sizeof(struct SpriteVertex), (void *)offsetof(struct SpriteVertex, sprite));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    glGenVertexArrays(1, &sParticleVao);
    glGenBuffers(1, &sParticleVbo);
    glBindVertexArray(sParticleVao);
    glBindBuffer(GL_ARRAY_BUFFER, sParticleVbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct ParticleVertex), (void *)offsetof(struct ParticleVertex, center));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct ParticleVertex), (void *)offsetof(struct ParticleVertex, corner));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(struct ParticleVertex), (void *)offsetof(struct ParticleVertex, sizeAngle));
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(struct ParticleVertex), (void *)offsetof(struct ParticleVertex, color));
    glVertexAttribIPointer(4, 2, GL_UNSIGNED_BYTE, sizeof(struct ParticleVertex), (void *)offsetof(struct ParticleVertex, shape));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glEnableVertexAttribArray(4);

    glGenVertexArrays(1, &sPostVao);
    glBindVertexArray(0);

    // The blur chain works at the GBA resolution and half of it, whatever the output size
    MakeTarget(&sDown1, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    MakeTarget(&sTemp1, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    MakeTarget(&sBlur1, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    MakeTarget(&sDown2, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2);
    MakeTarget(&sTemp2, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2);
    MakeTarget(&sBlur2, DISPLAY_WIDTH / 2, DISPLAY_HEIGHT / 2);

    if (glGetError() != GL_NO_ERROR)
    {
        fprintf(stderr, "field3d: GL error during setup\n");
        return false;
    }
    return true;
}

static void FreeSceneTarget(void)
{
    if (sSceneFbo == 0)
        return;
    glDeleteFramebuffers(1, &sSceneFbo);
    glDeleteTextures(1, &sSceneColor);
    glDeleteTextures(1, &sSceneDepth);
    FreeTarget(&sFinal);
    sSceneFbo = 0;
}

void GlField3D_Shutdown(void)
{
    FreeSceneTarget();
    FreeTarget(&sDown1);
    FreeTarget(&sTemp1);
    FreeTarget(&sBlur1);
    FreeTarget(&sDown2);
    FreeTarget(&sTemp2);
    FreeTarget(&sBlur2);
    glDeleteFramebuffers(1, &sShadowFbo);
    glDeleteTextures(1, &sShadowMap);
    glDeleteTextures(1, &sBgTiles);
    glDeleteTextures(1, &sObjTiles);
    glDeleteTextures(1, &sPalette);
    glDeleteTextures(1, &sMetatileTexture);
    glDeleteBuffers(1, &sMapVbo);
    glDeleteBuffers(1, &sSpriteVbo);
    glDeleteVertexArrays(1, &sMapVao);
    glDeleteVertexArrays(1, &sSpriteVao);
    glDeleteVertexArrays(1, &sPostVao);
    glDeleteVertexArrays(1, &sParticleVao);
    glDeleteBuffers(1, &sParticleVbo);
    glDeleteProgram(sParticleProgram);
    glDeleteProgram(sMap.program);
    glDeleteProgram(sMapShadow.program);
    glDeleteProgram(sSprite.program);
    glDeleteProgram(sSpriteShadow.program);
    glDeleteProgram(sDownsampleProgram);
    glDeleteProgram(sBlurProgram);
    glDeleteProgram(sFinishProgram);
    free(sOverviewPixels);
    sOverviewPixels = NULL;
}


// Color + depth textures the scene is drawn into (depth is read by the depth of field)
static bool MakeSceneTarget(int width, int height, GLuint *fbo, GLuint *color, GLuint *depth)
{
    glActiveTexture(GL_TEXTURE0 + SPARE_TEXTURE_UNIT);
    glGenTextures(1, color);
    glBindTexture(GL_TEXTURE_2D, *color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    *depth = DepthTexture(width, height, false);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *color, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, *depth, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        fprintf(stderr, "field3d: scene framebuffer incomplete\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

static void ResizeTarget(int width, int height)
{
    if (width == sWidth && height == sHeight)
        return;
    FreeSceneTarget();
    MakeSceneTarget(width, height, &sSceneFbo, &sSceneColor, &sSceneDepth);
    MakeTarget(&sFinal, width, height);
    sWidth = width;
    sHeight = height;
}

// --- matrices (column-major 4x4) -------------------------------------------

static void Multiply(float out[16], const float a[16], const float b[16])
{
    float r[16];
    int i, j, k;

    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
        {
            r[j * 4 + i] = 0;
            for (k = 0; k < 4; k++)
                r[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
        }
    memcpy(out, r, sizeof(r));
}

static void Normalize(float v[3])
{
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
}

static void Cross(float out[3], const float a[3], const float b[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float Dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// View matrix from an orthonormal basis (right, up, forward) and an eye point
static void BasisView(float view[16], const float s[3], const float u[3], const float f[3], const float eye[3])
{
    memset(view, 0, 16 * sizeof(float));
    view[0] = s[0]; view[4] = s[1]; view[8] = s[2];
    view[1] = u[0]; view[5] = u[1]; view[9] = u[2];
    view[2] = -f[0]; view[6] = -f[1]; view[10] = -f[2];
    view[12] = -Dot(s, eye);
    view[13] = -Dot(u, eye);
    view[14] = Dot(f, eye);
    view[15] = 1;
}

static void Basis(const float forward[3], float s[3], float u[3], float f[3])
{
    static const float sUp[3] = {0, 1, 0}, sNorth[3] = {0, 0, -1};

    memcpy(f, forward, 3 * sizeof(float));
    Normalize(f);
    Cross(s, f, fabsf(f[1]) > 0.99f ? sNorth : sUp);
    Normalize(s);
    Cross(u, s, f);
}

static void Ortho(float out[16], float l, float r, float b, float t, float n, float f)
{
    memset(out, 0, 16 * sizeof(float));
    out[0] = 2 / (r - l);
    out[5] = 2 / (t - b);
    out[10] = -2 / (f - n);
    out[12] = -(r + l) / (r - l);
    out[13] = -(t + b) / (t - b);
    out[14] = -(f + n) / (f - n);
    out[15] = 1;
}

static void CameraForward(float f[3])
{
    float pitch = CAMERA_PITCH_DEG * (float)M_PI / 180.0f;
    f[0] = 0;
    f[1] = -sinf(pitch);
    f[2] = -cosf(pitch);
}

static void CameraAxes(float right[3], float up[3])
{
    float forward[3], f[3];

    CameraForward(forward);
    Basis(forward, right, up, f);
}

// Blocks visible vertically at the target, by the menu's zoom (far, normal,
// near). Closer shows less of the far side of the map, where the field is
// only guessed (past the connected maps, the border).
static float CameraViewRows(void)
{
    static const float sRows[HD_ZOOM_COUNT] = {10.5f, 9.0f, 7.5f};
    int zoom = HdSettings_GetValue(HD_OPT_ZOOM);

    return sRows[zoom >= 0 && zoom < HD_ZOOM_COUNT ? zoom : HD_ZOOM_NORMAL];
}

static float CameraDistance(void)
{
    float fovy = CAMERA_FOVY_DEG * (float)M_PI / 180.0f;
    return (CameraViewRows() * 0.5f) / tanf(fovy * 0.5f);
}

static void CameraViewProjection(const float target[3], float aspect, float out[16])
{
    float fovy = CAMERA_FOVY_DEG * (float)M_PI / 180.0f;
    float distance = CameraDistance();
    float forward[3], s[3], u[3], f[3], eye[3], view[16], proj[16], t;

    CameraForward(forward);
    Basis(forward, s, u, f);
    eye[0] = target[0] - f[0] * distance;
    eye[1] = target[1] - f[1] * distance;
    eye[2] = target[2] - f[2] * distance;
    BasisView(view, s, u, f, eye);

    t = 1.0f / tanf(fovy * 0.5f);
    memset(proj, 0, sizeof(proj));
    proj[0] = t / aspect;
    proj[5] = t;
    proj[10] = (CAMERA_FAR + CAMERA_NEAR) / (CAMERA_NEAR - CAMERA_FAR);
    proj[11] = -1;
    proj[14] = 2 * CAMERA_FAR * CAMERA_NEAR / (CAMERA_NEAR - CAMERA_FAR);
    Multiply(out, proj, view);
}

// Orthographic sun view over the box [lo, hi]. The box center is moved in
// whole shadow texels, so shadows do not crawl while the camera scrolls.
static void SunViewProjection(const struct LightSetup *setup, const float lo[3], const float hi[3], float out[16],
                              float *texel)
{
    float s[3], u[3], f[3], eye[3], view[16], proj[16];
    float center[3], ext[3] = {0, 0, 0}, cs, cu, cf, step;
    int i;

    Basis(setup->sunDir, s, u, f);
    for (i = 0; i < 3; i++)
        center[i] = (lo[i] + hi[i]) * 0.5f;
    // half extents of the box seen from the sun
    for (i = 0; i < 8; i++)
    {
        float c[3] = {(i & 1) ? hi[0] - center[0] : lo[0] - center[0],
                      (i & 2) ? hi[1] - center[1] : lo[1] - center[1],
                      (i & 4) ? hi[2] - center[2] : lo[2] - center[2]};
        ext[0] = fmaxf(ext[0], fabsf(Dot(s, c)));
        ext[1] = fmaxf(ext[1], fabsf(Dot(u, c)));
        ext[2] = fmaxf(ext[2], fabsf(Dot(f, c)));
    }
    ext[0] = ext[1] = fmaxf(ext[0], ext[1]);
    step = 2 * ext[0] / SHADOW_SIZE;
    cs = floorf(Dot(s, center) / step) * step;
    cu = floorf(Dot(u, center) / step) * step;
    cf = Dot(f, center);
    for (i = 0; i < 3; i++)
        eye[i] = s[i] * cs + u[i] * cu + f[i] * (cf - ext[2] - 1.0f);
    BasisView(view, s, u, f, eye);
    Ortho(proj, -ext[0], ext[0], -ext[1], ext[1], 0.0f, 2 * ext[2] + 2.0f);
    Multiply(out, proj, view);
    *texel = 1.0f / SHADOW_SIZE;
}

// --- drawing ---------------------------------------------------------------

static void UploadDynamicData(const struct Field3DFrame *frame)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sBgTiles);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 128, GL_RED_INTEGER, GL_UNSIGNED_BYTE, (const void *)VRAM);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, sObjTiles);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 128, GL_RED_INTEGER, GL_UNSIGNED_BYTE, (const void *)OBJ_VRAM0);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, sPalette);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 16, 32, GL_RED_INTEGER, GL_UNSIGNED_SHORT, (const void *)PLTT);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, sMetatileTexture);
    if (frame->metatileVersion != sMetatileVersion)
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, FIELD3D_METATILE_ROWS, GL_RED_INTEGER, GL_UNSIGNED_SHORT,
                        frame->metatileEntries);
        sMetatileVersion = frame->metatileVersion;
    }
    if (frame->meshVersion != sMeshVersion)
    {
        glBindBuffer(GL_ARRAY_BUFFER, sMapVbo);
        glBufferData(GL_ARRAY_BUFFER, frame->vertexCount * sizeof(struct Field3DVertex), frame->vertices, GL_STATIC_DRAW);
        sMapVertexCount = frame->vertexCount;
        sMeshVersion = frame->meshVersion;
        sPatchVersion = frame->patchVersion;
    }
    else if (frame->patchVersion != sPatchVersion)
    {
        // door frames: a few cells changed texture rows
        glBindBuffer(GL_ARRAY_BUFFER, sMapVbo);
        if (frame->patchCount > 0 && frame->patchFirst + frame->patchCount <= sMapVertexCount)
            glBufferSubData(GL_ARRAY_BUFFER, frame->patchFirst * sizeof(struct Field3DVertex),
                            frame->patchCount * sizeof(struct Field3DVertex), frame->vertices + frame->patchFirst);
        sPatchVersion = frame->patchVersion;
    }
}

static void UploadBillboards(const struct Field3DFrame *frame)
{
    static struct SpriteVertex vertices[FIELD3D_MAX_BILLBOARDS * 6];
    static const int sOrder[6] = {0, 1, 2, 1, 3, 2};
    int i, n = 0;

    for (i = 0; i < frame->billboardCount; i++)
    {
        const struct Field3DBillboard *bb = &frame->billboards[i];
        // a paper figure's quad is a pixel wider on its outer sides, for the rim
        float px = 1.0f / 16.0f;
        float left = (bb->edges & FIELD3D_EDGE_LEFT) ? 1 : 0, right = (bb->edges & FIELD3D_EDGE_RIGHT) ? 1 : 0;
        float top = (bb->edges & FIELD3D_EDGE_TOP) ? 1 : 0;
        float corners[4][5] = {
            {bb->x0 - left * px, bb->y1 + top * px, bb->z, -left, bb->cutTop - top},
            {bb->x1 + right * px, bb->y1 + top * px, bb->z, bb->width + right, bb->cutTop - top},
            {bb->x0 - left * px, bb->y0, bb->z, -left, bb->height},
            {bb->x1 + right * px, bb->y0, bb->z, bb->width + right, bb->height},
        };
        int k;

        if (bb->tilt != 0.0f)
        {
            // rocking in its own plane around the feet
            float c = cosf(bb->tilt), s = sinf(bb->tilt);
            for (k = 0; k < 4; k++)
            {
                float dx = corners[k][0] - bb->pivotX, dy = corners[k][1] - bb->pivotY;
                corners[k][0] = bb->pivotX + dx * c - dy * s;
                corners[k][1] = bb->pivotY + dx * s + dy * c;
            }
        }
        if (bb->flags & FIELD3D_BB_FLAT)
        {
            // lying at height y0, from Z z (top of the image) to y1
            const float flat[4][5] = {
                {bb->x0, bb->y0, bb->z, 0, 0},
                {bb->x1, bb->y0, bb->z, bb->width, 0},
                {bb->x0, bb->y0, bb->y1, 0, bb->height},
                {bb->x1, bb->y0, bb->y1, bb->width, bb->height},
            };
            memcpy(corners, flat, sizeof(corners));
        }
        for (k = 0; k < 6; k++)
        {
            struct SpriteVertex *v = &vertices[n++];
            const float *c = corners[sOrder[k]];
            v->pos[0] = c[0];
            v->pos[1] = c[1];
            v->pos[2] = c[2];
            v->local[0] = c[3];
            v->local[1] = c[4];
            v->sprite[0] = bb->tileNum;
            v->sprite[1] = bb->width | (bb->height << 8);
            v->sprite[2] = bb->palette;
            v->sprite[3] = bb->flags;
        }
    }
    sSpriteVertexCount = n;
    if (n == 0)
        return;
    glBindBuffer(GL_ARRAY_BUFFER, sSpriteVbo);
    glBufferData(GL_ARRAY_BUFFER, n * sizeof(struct SpriteVertex), vertices, GL_DYNAMIC_DRAW);
}

static void SetSceneUniforms(const struct SceneProgram *p, const struct Field3DFrame *frame, const struct LightSetup *setup,
                             const struct View *view, const float *viewProj, float sunScale, bool shadows)
{
    glUseProgram(p->program);
    glUniformMatrix4fv(p->viewProj, 1, GL_FALSE, viewProj);
    glUniformMatrix4fv(p->lightViewProj, 1, GL_FALSE, view->lightViewProj);
    glUniform3f(p->toSun, view->toSun[0], view->toSun[1], view->toSun[2]);
    if (p->fade >= 0)
        glUniform2f(p->fade, frame->fade[0], frame->fade[1]);
    if (p->time >= 0)
    {
        glUniform1f(p->time, frame->time);
        glUniform1f(p->wind, setup->wind);
        glUniform1i(p->actorCount, frame->actorCount);
        if (frame->actorCount > 0)
            glUniform2fv(p->actors, frame->actorCount, &frame->actors[0][0]);
    }
    if (p->ambient >= 0)
    {
        glUniform3f(p->ambient, setup->ambient[0] + setup->sun[0] * (1 - sunScale),
                    setup->ambient[1] + setup->sun[1] * (1 - sunScale), setup->ambient[2] + setup->sun[2] * (1 - sunScale));
        glUniform3f(p->sun, setup->sun[0] * sunScale, setup->sun[1] * sunScale, setup->sun[2] * sunScale);
        glUniform1f(p->facade, setup->facade);
        glUniform1f(p->shadowTexel, 1.0f / SHADOW_SIZE);
        glUniform1i(p->shadowsOn, shadows);
    }
}

// Map and billboards, either lit (into the bound color target) or as depth
// from the sun (into the shadow map)
static void DrawScene(const struct Field3DFrame *frame, const struct LightSetup *setup, const struct View *view,
                      bool shadowPass, float sunScale, bool shadows)
{
    const struct SceneProgram *map = shadowPass ? &sMapShadow : &sMap;
    const struct SceneProgram *sprite = shadowPass ? &sSpriteShadow : &sSprite;
    const float *viewProj = shadowPass ? view->lightViewProj : view->viewProj;

    SetSceneUniforms(map, frame, setup, view, viewProj, sunScale, shadows);
    glBindVertexArray(sMapVao);
    // The depth offset keeps the ground from shadowing itself; on people it
    // would push their shadow away from the feet (uToSun already keeps a
    // billboard from shadowing itself)
    if (shadowPass)
        glEnable(GL_POLYGON_OFFSET_FILL);
    glDrawArrays(GL_TRIANGLES, 0, sMapVertexCount);
    glDisable(GL_POLYGON_OFFSET_FILL);
    if (sSpriteVertexCount > 0)
    {
        SetSceneUniforms(sprite, frame, setup, view, viewProj, sunScale, shadows);
        glBindVertexArray(sSpriteVao);
        glDrawArrays(GL_TRIANGLES, 0, sSpriteVertexCount);
    }
}

// Particles, after the opaque scene: depth tested, not written
static void DrawParticles(const struct Field3DFrame *frame, const struct View *view)
{
    static struct ParticleVertex vertices[FIELD3D_MAX_PARTICLES * 6];
    static const float sCorners[6][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, 1}, {1, -1}, {-1, -1}};
    int i, k, n = 0;

    for (i = 0; i < frame->particleCount && i < FIELD3D_MAX_PARTICLES; i++)
    {
        const struct Field3DParticle *p = &frame->particles[i];
        for (k = 0; k < 6; k++)
        {
            struct ParticleVertex *v = &vertices[n++];
            memcpy(v->center, p->pos, sizeof(v->center));
            v->corner[0] = sCorners[k][0];
            v->corner[1] = sCorners[k][1];
            v->sizeAngle[0] = p->size;
            v->sizeAngle[1] = p->angle;
            memcpy(v->color, p->color, sizeof(v->color));
            v->shape = p->shape;
            v->flags = p->flags;
        }
    }
    if (n == 0)
        return;
    glBindVertexArray(sParticleVao);
    glBindBuffer(GL_ARRAY_BUFFER, sParticleVbo);
    glBufferData(GL_ARRAY_BUFFER, n * sizeof(struct ParticleVertex), vertices, GL_DYNAMIC_DRAW);
    glUseProgram(sParticleProgram);
    glUniformMatrix4fv(sParticleViewProjLoc, 1, GL_FALSE, view->viewProj);
    glUniform3f(sParticleRightLoc, view->right[0], view->right[1], view->right[2]);
    glUniform3f(sParticleUpLoc, view->up[0], view->up[1], view->up[2]);
    glUniform2f(sParticleFadeLoc, frame->fade[0], frame->fade[1]);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDrawArrays(GL_TRIANGLES, 0, n);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

// Shadow map, then the lit scene (and the particles) into fbo (width x height)
static void RenderLit(const struct Field3DFrame *frame, const struct LightSetup *setup, const struct View *view, GLuint fbo,
                      int width, int height, bool particles)
{
    uint16_t backdrop = *(const uint16_t *)PLTT;
    float r = (backdrop & 31) / 31.0f, g = ((backdrop >> 5) & 31) / 31.0f, b = ((backdrop >> 10) & 31) / 31.0f;
    // Overcast weather: weaker sun, more ambient; light off: flat, like the GBA
    float sunScale = HdSettings_Get(HD_OPT_LIGHTING) ? 1.0f - 0.75f * frame->dim : 0.0f;
    bool shadows = sunScale > 0.0f && HdSettings_Get(HD_OPT_SHADOWS);

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);

    if (shadows)
    {
        glBindFramebuffer(GL_FRAMEBUFFER, sShadowFbo);
        glViewport(0, 0, SHADOW_SIZE, SHADOW_SIZE);
        glClearDepthf(1.0f);
        glClear(GL_DEPTH_BUFFER_BIT);
        glPolygonOffset(1.5f, 3.0f);
        DrawScene(frame, setup, view, true, sunScale, true);
    }
    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, sShadowMap);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, width, height);
    glClearColor(r, g, b, 1.0f);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (sunScale > 0.0f)
    {
        DrawScene(frame, setup, view, false, sunScale, shadows);
    }
    else
    {
        // flat: ambient 1, no sun (the shadow map is not read)
        static const struct LightSetup sFlat = {.sunDir = {0, -1, 0}, .ambient = {1, 1, 1}, .facade = 1};
        DrawScene(frame, &sFlat, view, false, 1.0f, false);
    }
    if (particles)
        DrawParticles(frame, view);
    glDisable(GL_DEPTH_TEST);
}

static void PostPass(GLuint program, const struct Target *dest, GLuint source)
{
    glBindFramebuffer(GL_FRAMEBUFFER, dest->fbo);
    glViewport(0, 0, dest->width, dest->height);
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glBindVertexArray(sPostVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void Downsample(const struct Target *dest, GLuint source, int sourceWidth, int sourceHeight)
{
    int taps = (sourceWidth / dest->width + 1) / 2;

    glUseProgram(sDownsampleProgram);
    glUniform2f(sDownsampleTexelLoc, 1.0f / sourceWidth, 1.0f / sourceHeight);
    glUniform1i(sDownsampleTapsLoc, taps < 1 ? 1 : taps);
    PostPass(sDownsampleProgram, dest, source);
}

static void Blur(const struct Target *dest, const struct Target *temp, const struct Target *source)
{
    glUseProgram(sBlurProgram);
    glUniform2f(sBlurStepLoc, 1.0f / source->width, 0.0f);
    PostPass(sBlurProgram, temp, source->texture);
    glUseProgram(sBlurProgram);
    glUniform2f(sBlurStepLoc, 0.0f, 1.0f / source->height);
    PostPass(sBlurProgram, dest, temp->texture);
}

// Blur, glow and haze only in the open air (not in houses or caves)
static bool Effect(const struct Field3DFrame *frame, int option)
{
    return frame->openAir && HdSettings_Get(option);
}

static void PostProcess(const struct Field3DFrame *frame, const struct LightSetup *setup)
{
    // Blurred copies at the GBA resolution and half of it
    Downsample(&sDown1, sSceneColor, sWidth, sHeight);
    Blur(&sBlur1, &sTemp1, &sDown1);
    Downsample(&sDown2, sBlur1.texture, sBlur1.width, sBlur1.height);
    Blur(&sBlur2, &sTemp2, &sDown2);

    glUseProgram(sFinishProgram);
    glUniform2f(sFinishClipLoc, CAMERA_NEAR, CAMERA_FAR);
    glUniform1f(sFinishFocusLoc, CameraDistance());
    glUniform4f(sFinishDofRangeLoc, 2.0f, 4.0f, 3.0f, 8.0f);
    glUniform1f(sFinishDofLoc, Effect(frame, HD_OPT_DOF) ? setup->dof : 0.0f);
    glUniform1f(sFinishBloomLoc, Effect(frame, HD_OPT_BLOOM) ? setup->bloom : 0.0f);
    if (Effect(frame, HD_OPT_GRADING))
    {
        // the haze fades with the screen, like the palettes
        float dark = frame->fade[0], white = frame->fade[1];
        glUniform1f(sFinishVignetteLoc, setup->vignette);
        glUniform4f(sFinishFogLoc, setup->fog[0] * dark + (1 - setup->fog[0] * dark) * white,
                    setup->fog[1] * dark + (1 - setup->fog[1] * dark) * white,
                    setup->fog[2] * dark + (1 - setup->fog[2] * dark) * white, setup->fog[3]);
        glUniform2f(sFinishGradeLoc, setup->saturation, setup->contrast);
    }
    else
    {
        glUniform1f(sFinishVignetteLoc, 0.0f);
        glUniform4f(sFinishFogLoc, 0.0f, 0.0f, 0.0f, 0.0f);
        glUniform2f(sFinishGradeLoc, 1.0f, 1.0f);
    }
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, sSceneDepth);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, sBlur1.texture);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D, sBlur2.texture);
    PostPass(sFinishProgram, &sFinal, sSceneColor);
}

// The outdoor light through the day (hour of the game's clock). The day is
// sLightSetups[FIELD3D_LIGHT_OUTDOOR]; the sun rises in the east and sets in
// the west (sunDir is the way the light travels), the moon lights the night.
static const struct LightSetup sNight = {
    .sunDir = {0.30f, -1.0f, 0.50f}, .sun = {0.13f, 0.15f, 0.24f}, .ambient = {0.34f, 0.39f, 0.60f},
    .facade = 0.8f, .wind = 0.05f, .dof = 0.85f, .bloom = 0.22f, .vignette = 0.36f,
    .fog = {0.08f, 0.11f, 0.22f, 0.30f}, .saturation = 0.82f, .contrast = 1.05f,
};
static const struct LightSetup sDawn = {
    .sunDir = {-0.85f, -1.0f, 0.30f}, .sun = {0.58f, 0.40f, 0.26f}, .ambient = {0.50f, 0.47f, 0.58f},
    .facade = 0.8f, .wind = 0.06f, .dof = 0.85f, .bloom = 0.18f, .vignette = 0.26f,
    .fog = {0.96f, 0.80f, 0.70f, 0.20f}, .saturation = 1.02f, .contrast = 1.04f,
};
static const struct LightSetup sDusk = {
    .sunDir = {0.85f, -1.0f, 0.30f}, .sun = {0.62f, 0.36f, 0.22f}, .ambient = {0.48f, 0.41f, 0.55f},
    .facade = 0.8f, .wind = 0.06f, .dof = 0.85f, .bloom = 0.20f, .vignette = 0.28f,
    .fog = {0.97f, 0.70f, 0.56f, 0.22f}, .saturation = 1.04f, .contrast = 1.04f,
};

static float Mix(float a, float b, float t)
{
    return a + (b - a) * t;
}

static void MixSetups(const struct LightSetup *a, const struct LightSetup *b, float t, struct LightSetup *out)
{
    int i;

    for (i = 0; i < 3; i++)
    {
        out->sunDir[i] = Mix(a->sunDir[i], b->sunDir[i], t);
        out->sun[i] = Mix(a->sun[i], b->sun[i], t);
        out->ambient[i] = Mix(a->ambient[i], b->ambient[i], t);
    }
    for (i = 0; i < 4; i++)
        out->fog[i] = Mix(a->fog[i], b->fog[i], t);
    out->facade = Mix(a->facade, b->facade, t);
    out->wind = Mix(a->wind, b->wind, t);
    out->dof = Mix(a->dof, b->dof, t);
    out->bloom = Mix(a->bloom, b->bloom, t);
    out->vignette = Mix(a->vignette, b->vignette, t);
    out->saturation = Mix(a->saturation, b->saturation, t);
    out->contrast = Mix(a->contrast, b->contrast, t);
}

static void TimeOfDay(float hour, struct LightSetup *out)
{
    static const struct { float hour; const struct LightSetup *setup; } sKeys[] = {
        {0.0f, &sNight}, {5.0f, &sNight}, {6.5f, &sDawn}, {8.5f, &sLightSetups[FIELD3D_LIGHT_OUTDOOR]},
        {16.5f, &sLightSetups[FIELD3D_LIGHT_OUTDOOR]}, {18.5f, &sDusk}, {20.0f, &sNight}, {24.0f, &sNight},
    };
    int i;

    for (i = 0; i + 1 < (int)(sizeof(sKeys) / sizeof(sKeys[0])); i++)
    {
        if (hour <= sKeys[i + 1].hour)
        {
            float t = (hour - sKeys[i].hour) / (sKeys[i + 1].hour - sKeys[i].hour);
            MixSetups(sKeys[i].setup, sKeys[i + 1].setup, t < 0 ? 0 : t, out);
            return;
        }
    }
    *out = sNight;
}

// hour < 0: daylight (map overviews are always taken by day)
static void SetupFor(const struct Field3DFrame *frame, float hour, struct LightSetup *out)
{
    int lighting = frame->lighting;
    if (lighting < 0 || lighting >= (int)(sizeof(sLightSetups) / sizeof(sLightSetups[0])))
        lighting = FIELD3D_LIGHT_OUTDOOR;
    if (lighting == FIELD3D_LIGHT_OUTDOOR && hour >= 0)
        TimeOfDay(hour, out);
    else
        *out = sLightSetups[lighting];
}

static void SetToSun(struct View *view, const struct LightSetup *setup)
{
    float d[3] = {setup->sunDir[0], setup->sunDir[1], setup->sunDir[2]};
    Normalize(d);
    view->toSun[0] = -d[0] * 0.04f;
    view->toSun[1] = -d[1] * 0.04f;
    view->toSun[2] = -d[2] * 0.04f;
}

unsigned GlField3D_Render(const struct Field3DFrame *frame, int width, int height)
{
    struct LightSetup setupForTime;
    const struct LightSetup *setup = &setupForTime;
    struct View view;
    float lo[3], hi[3], texel;

    SetupFor(frame, frame->hour, &setupForTime);
    ResizeTarget(width, height);
    UploadDynamicData(frame);
    UploadBillboards(frame);

    CameraViewProjection(frame->target, (float)width / height, view.viewProj);
    CameraAxes(view.right, view.up);
    lo[0] = frame->target[0] - SHADOW_BOX_X;
    hi[0] = frame->target[0] + SHADOW_BOX_X;
    lo[1] = frame->target[1] - 1.0f;
    hi[1] = frame->target[1] + SHADOW_BOX_TOP;
    lo[2] = frame->target[2] - SHADOW_BOX_NORTH;
    hi[2] = frame->target[2] + SHADOW_BOX_SOUTH;
    SunViewProjection(setup, lo, hi, view.lightViewProj, &texel);
    SetToSun(&view, setup);

    RenderLit(frame, setup, &view, sSceneFbo, width, height, true);
    if (!Effect(frame, HD_OPT_DOF) && !Effect(frame, HD_OPT_BLOOM) && !Effect(frame, HD_OPT_GRADING))
    {
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return sSceneColor;
    }
    PostProcess(frame, setup);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return sFinal.texture;
}

const uint8_t *GlField3D_RenderOverview(const struct Field3DFrame *frame, int pixelsPerBlock, int *outWidth, int *outHeight)
{
    struct LightSetup setupForTime;
    const struct LightSetup *setup = &setupForTime;
    struct View view;
    float forward[3], s[3], u[3], f[3], eye[3] = {0, 0, 0}, viewMatrix[16], proj[16], texel;
    float lo[3], hi[3], vmin[3] = {1e9f, 1e9f, 1e9f}, vmax[3] = {-1e9f, -1e9f, -1e9f};
    GLuint fbo, color, depth;
    GLint maxSize = 4096;
    int i, width, height, row;
    uint8_t *rgba;

    SetupFor(frame, -1.0f, &setupForTime);
    lo[0] = frame->mapMin[0] - OVERVIEW_MARGIN;
    hi[0] = frame->mapMax[0] + OVERVIEW_MARGIN;
    lo[1] = 0.0f;
    hi[1] = OVERVIEW_TOP;
    lo[2] = frame->mapMin[1] - OVERVIEW_MARGIN;
    hi[2] = frame->mapMax[1] + OVERVIEW_MARGIN;

    // The game camera's angle, orthographic, framing the whole map
    CameraForward(forward);
    Basis(forward, s, u, f);
    BasisView(viewMatrix, s, u, f, eye);
    for (i = 0; i < 8; i++)
    {
        float c[3] = {(i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]};
        float v[3] = {Dot(s, c), Dot(u, c), -Dot(f, c)};
        int k;
        for (k = 0; k < 3; k++)
        {
            vmin[k] = fminf(vmin[k], v[k]);
            vmax[k] = fmaxf(vmax[k], v[k]);
        }
    }
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    while (pixelsPerBlock > 1 && ((vmax[0] - vmin[0]) * pixelsPerBlock > maxSize || (vmax[1] - vmin[1]) * pixelsPerBlock > maxSize))
        pixelsPerBlock--;
    width = (int)ceilf((vmax[0] - vmin[0]) * pixelsPerBlock);
    height = (int)ceilf((vmax[1] - vmin[1]) * pixelsPerBlock);
    Ortho(proj, vmin[0], vmin[0] + (float)width / pixelsPerBlock, vmin[1], vmin[1] + (float)height / pixelsPerBlock,
          -vmax[2] - 1.0f, -vmin[2] + 1.0f);
    Multiply(view.viewProj, proj, viewMatrix);
    SunViewProjection(setup, lo, hi, view.lightViewProj, &texel);
    SetToSun(&view, setup);

    if (!MakeSceneTarget(width, height, &fbo, &color, &depth))
        return NULL;
    UploadDynamicData(frame);
    UploadBillboards(frame);
    CameraAxes(view.right, view.up);
    RenderLit(frame, setup, &view, fbo, width, height, false);

    rgba = malloc((size_t)width * height * 4);
    free(sOverviewPixels);
    sOverviewPixels = malloc((size_t)width * height * 3);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    for (row = 0; row < height; row++)
    {
        const uint8_t *src = &rgba[(size_t)(height - 1 - row) * width * 4];
        uint8_t *dst = &sOverviewPixels[(size_t)row * width * 3];
        int x;
        for (x = 0; x < width; x++)
        {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
    free(rgba);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &color);
    glDeleteTextures(1, &depth);
    *outWidth = width;
    *outHeight = height;
    return sOverviewPixels;
}

#endif // PORTABLE
