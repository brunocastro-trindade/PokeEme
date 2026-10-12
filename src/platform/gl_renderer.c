#ifdef PORTABLE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "platform/gl_api.h"
#include "platform/gl_field3d.h"
#include "platform/gl_renderer.h"

#define MAX_COMPOSE_SCALE 8

// Fullscreen triangle; vUV covers [0,1] over the viewport.
static const char sFullscreenVS[] =
    "#version 300 es\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    vUV = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// The GBA blend stage, a line-by-line port of ComposeScanline() and
// alphaBlendSelectTargetB() in gba_easy_draw.c. Keep them in sync: the
// harness (PE_VERIFY_GL=1) checks both produce the same pixels.
//
// HD sprites come in as one more sprite candidate per pixel (uHdColor and
// uHdMeta, at compose resolution). They take part in priorities and are
// blended like the sprites DrawSprites() blends.
static const char sComposeFS[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2D;\n"
    "precision highp usampler2D;\n"
    "precision highp usampler2DArray;\n"
    "uniform usampler2DArray uLayers;\n"
    "uniform usampler2DArray uObjIndex;\n"
    "uniform usampler2D uWinMask;\n"
    "uniform usampler2D uLineRegs;\n"
    "uniform sampler2D uHdColor;\n"
    "uniform usampler2D uHdMeta;\n"
    "uniform sampler2D uScene;\n"
    "uniform int uScale;\n"
    "uniform bool uHdEnabled;\n"
    "uniform bool uField3D;\n"
    "uniform bool uSceneWave;\n"    // the map scroll changes from line to line
    "uniform vec2 uSceneScroll;\n" // the scroll the scene was rendered for
    "out vec4 fragColor;\n"
    "\n"
    "// Colors are vec4: rgb in GBA 5-bit units (0..31), a = 1 where drawn. PPU\n"
    "// layers hold whole numbers and the math floors like the C code does, so\n"
    "// their result is exact; the 3D scene brings fractions and keeps them.\n"
    "const uint BLDCNT_TGT1_OBJ = 0x10u;\n"
    "const uint BLDCNT_TGT1_BD = 0x20u;\n"
    "const uint BLDCNT_TGT2_OBJ = 0x1000u;\n"
    "const uint BLDCNT_TGT2_BD = 0x2000u;\n"
    "const uint WINMASK_OBJ = 0x10u;\n"
    "const uint WINMASK_CLR = 0x20u;\n"
    "const uint HD_PRESENT = 0x8000u;\n"
    "const uint HD_SEMI_TRANSPARENT = 0x200u;\n"
    "const uint HD_MOSAIC = 0x400u;\n"
    "\n"
    "ivec2 gPos, gOut;\n"
    "uint gDispcnt, gBldcnt, gBldalpha, gBldy, gBgPrio, gMosaic, gMask, gBlendMode;\n"
    "vec4 gBackdrop;\n"
    "bool gWindows;\n"
    "vec4 gSpriteTop[4];\n"
    "\n"
    "vec4 unpack555(uint c) {\n"
    "    return vec4(float(c & 31u), float((c >> 5) & 31u), float((c >> 10) & 31u), (c & 0x8000u) != 0u ? 1.0 : 0.0);\n"
    "}\n"
    "// Where to read the scene: shifted by per-line map scroll changes, and in\n"
    "// blocks when the map has mosaic on (battle transitions)\n"
    "ivec2 sceneTexel() {\n"
    "    ivec2 p = gOut;\n"
    "    uvec4 r2 = texelFetch(uLineRegs, ivec2(2, 159 - gOut.y / uScale), 0);\n"
    "    if (uSceneWave) {\n"
    "        int dx = ((int(r2.x) - int(uSceneScroll.x) + 256) & 511) - 256;\n"
    "        int dy = ((int(r2.y) - int(uSceneScroll.y) + 256) & 511) - 256;\n"
    "        p += ivec2(dx, -dy) * uScale;\n"
    "    }\n"
    "    uint mosaic = gMosaic & 255u;\n"
    "    if ((r2.z & 0x40u) != 0u && mosaic != 0u) {\n"
    "        int mx = int(mosaic & 15u) + 1, my = int(mosaic >> 4) + 1;\n"
    "        ivec2 g = ivec2(p.x / uScale, 159 - p.y / uScale);\n"
    "        g -= ivec2(g.x % mx, g.y % my);\n"
    "        p = ivec2(g.x * uScale + uScale / 2, (159 - g.y) * uScale + uScale / 2);\n"
    "    }\n"
    "    return clamp(p, ivec2(0), ivec2(240 * uScale - 1, 160 * uScale - 1));\n"
    "}\n"
    "// BG1-BG3 hold the map; with the 3D field the scene takes their place\n"
    "vec4 layerAt(int l) {\n"
    "    if (uField3D && l < 4 && l > 0) {\n"
    "        if (l != 3) return vec4(0.0);\n"
    "        return vec4(texelFetch(uScene, sceneTexel(), 0).rgb * 31.0, 1.0);\n"
    "    }\n"
    "    return unpack555(texelFetch(uLayers, ivec3(gPos, l), 0).r);\n"
    "}\n"
    "uint objIndexAt(int p) { return texelFetch(uObjIndex, ivec3(gPos, p), 0).r & 127u; }\n"
    "bool objRawAt(int p) { return (texelFetch(uObjIndex, ivec3(gPos, p), 0).r & 128u) != 0u; }\n"
    "bool drawn(vec4 c) { return c.a > 0.5; }\n"
    "bool bgEnabled(int bg) { return ((gDispcnt >> 8) & (1u << uint(bg))) != 0u; }\n"
    "int bgPriority(int bg) { return int((gBgPrio >> uint(2 * bg)) & 3u); }\n"
    "\n"
    "vec4 alphaBlend(vec4 a, vec4 b) {\n"
    "    float eva = float(gBldalpha & 31u), evb = float((gBldalpha >> 8) & 31u);\n"
    "    return vec4(min(floor((a.rgb * eva + b.rgb * evb) / 16.0), vec3(31.0)), 1.0);\n"
    "}\n"
    "// whole numbers (PPU layers) floor like the C code; fractions (3D scene) stay exact\n"
    "vec3 floorWhole(vec3 v, vec3 c) { return mix(v, floor(v), vec3(equal(fract(c), vec3(0.0)))); }\n"
    "vec4 brightnessUp(vec4 c) {\n"
    "    float evy = float(gBldy & 31u);\n"
    "    return vec4(min(c.rgb + floorWhole((31.0 - c.rgb) * evy / 16.0, c.rgb), vec3(31.0)), c.a);\n"
    "}\n"
    "vec4 brightnessDown(vec4 c) {\n"
    "    float evy = float(gBldy & 31u);\n"
    "    vec3 v = c.rgb - floorWhole(c.rgb * evy / 16.0, c.rgb);\n"
    "    v = mix(v, vec3(31.0), vec3(lessThan(v, vec3(0.0))) + vec3(greaterThan(v, vec3(31.0))));\n"
    "    return vec4(v, c.a);\n"
    "}\n"
    "\n"
    "// Second blend target for a layer at priority prnum. BGs of the same priority\n"
    "// with a higher number lie below a BG (they come later in the sorted list);\n"
    "// for sprites (curBg = -1) every BG of the same priority counts.\n"
    "bool selectTargetB(int prnum, int curBg, int numBgs, bool spriteBlend, out vec4 color) {\n"
    "    for (int p = prnum; p <= 3; p++) {\n"
    "        if (spriteBlend && drawn(gSpriteTop[p])) { color = gSpriteTop[p]; return true; }\n"
    "        for (int bg = 0; bg < numBgs; bg++) {\n"
    "            if (bgPriority(bg) != p || (p == prnum && bg <= curBg))\n"
    "                continue;\n"
    "            vec4 c = layerAt(bg);\n"
    "            if (drawn(c) && (gBldcnt & (1u << uint(8 + bg))) != 0u && bgEnabled(bg)) { color = c; return true; }\n"
    "            if (drawn(c) && bgEnabled(bg) && prnum != p)\n"
    "                return false;\n"
    "        }\n"
    "    }\n"
    "    if ((gBldcnt & BLDCNT_TGT2_BD) != 0u) { color = gBackdrop; return true; }\n"
    "    color = vec4(0.0);\n"
    "    return false;\n"
    "}\n"
    "\n"
    "// The blending DrawSprites() applies to a sprite pixel\n"
    "vec4 blendSprite(vec4 c, int p, bool semiTransparent, int numBgs) {\n"
    "    bool winBlend = !gWindows || (gMask & WINMASK_CLR) != 0u;\n"
    "    if ((gBlendMode == 1u && (gBldcnt & BLDCNT_TGT1_OBJ) != 0u && winBlend) || semiTransparent) {\n"
    "        vec4 b;\n"
    "        if (selectTargetB(p, -1, numBgs, false, b))\n"
    "            c = alphaBlend(c, b);\n"
    "    } else if ((gBldcnt & BLDCNT_TGT1_OBJ) != 0u && winBlend) {\n"
    "        if (gBlendMode == 2u) c = brightnessUp(c);\n"
    "        else if (gBlendMode == 3u) c = brightnessDown(c);\n"
    "    }\n"
    "    return c;\n"
    "}\n"
    "\n"
    "// 5-bit to 8-bit like (c << 3) | (c >> 2) for whole numbers; fractions scale linearly\n"
    "vec3 to8bit(vec3 c) {\n"
    "    vec3 exact = (c * 8.0 + floor(c / 4.0)) / 255.0;\n"
    "    return mix(c / 31.0, exact, vec3(equal(fract(c), vec3(0.0))));\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    gOut = ivec2(gl_FragCoord.xy);\n"
    "    int line = 159 - gOut.y / uScale;\n"
    "    gPos = ivec2(gOut.x / uScale, line);\n"
    "    uvec4 r0 = texelFetch(uLineRegs, ivec2(0, line), 0);\n"
    "    uvec4 r1 = texelFetch(uLineRegs, ivec2(1, line), 0);\n"
    "    gDispcnt = r0.x; gBldcnt = r0.y; gBldalpha = r0.z; gBldy = r0.w;\n"
    "    gBackdrop = vec4(unpack555(r1.x).rgb, 1.0); gBgPrio = r1.y; gMosaic = r1.w;\n"
    "    gWindows = (r1.z & 1u) != 0u;\n"
    "    gMask = texelFetch(uWinMask, gPos, 0).r;\n"
    "    gBlendMode = (gBldcnt >> 6) & 3u;\n"
    "    int numBgs = (gDispcnt & 3u) == 0u ? 4 : 3;\n"
    "\n"
    "    for (int p = 0; p < 4; p++)\n"
    "        gSpriteTop[p] = unpack555(texelFetch(uLayers, ivec3(gPos, 4 + p), 0).r);\n"
    "    // sprites the PPU left unblended (3D field): blend with what is below them here\n"
    "    for (int p = 0; p < 4; p++)\n"
    "        if (drawn(gSpriteTop[p]) && objRawAt(p))\n"
    "            gSpriteTop[p] = blendSprite(gSpriteTop[p], p, true, numBgs);\n"
    "\n"
    "    // An HD sprite replaces the top sprite of its priority when its OAM\n"
    "    // index is lower (or no PPU sprite is there)\n"
    "    if (uHdEnabled) {\n"
    "        ivec2 hdPos = gOut;\n"
    "        uint meta = texelFetch(uHdMeta, hdPos, 0).r;\n"
    "        if ((meta & HD_MOSAIC) != 0u) {\n"
    "            int mx = int(gMosaic >> 8) & 15, my = int(gMosaic >> 12) & 15;\n"
    "            ivec2 g = ivec2(gPos.x - gPos.x % (mx + 1), gPos.y - gPos.y % (my + 1));\n"
    "            hdPos = ivec2(g.x * uScale + uScale / 2, (159 - g.y) * uScale + uScale / 2);\n"
    "            meta = texelFetch(uHdMeta, hdPos, 0).r;\n"
    "        }\n"
    "        if ((meta & HD_PRESENT) != 0u) {\n"
    "            int p = int((meta >> 7) & 3u);\n"
    "            uint index = meta & 127u;\n"
    "            if (!drawn(gSpriteTop[p]) || index < objIndexAt(p)) {\n"
    "                vec3 c = floor(texelFetch(uHdColor, hdPos, 0).rgb * 255.0 / 8.0);\n"
    "                gSpriteTop[p] = blendSprite(vec4(c, 1.0), p, (meta & HD_SEMI_TRANSPARENT) != 0u, numBgs);\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "\n"
    "    vec4 color = gBackdrop;\n"
    "    if ((gBldcnt & BLDCNT_TGT1_BD) != 0u) {\n"
    "        if (gBlendMode == 2u) color = brightnessUp(color);\n"
    "        else if (gBlendMode == 3u) color = brightnessDown(color);\n"
    "    }\n"
    "\n"
    "    for (int p = 3; p >= 0; p--) {\n"
    "        for (int bg = numBgs - 1; bg >= 0; bg--) {\n"
    "            if (bgPriority(bg) != p || !bgEnabled(bg))\n"
    "                continue;\n"
    "            vec4 c = layerAt(bg);\n"
    "            if (!drawn(c))\n"
    "                continue;\n"
    "            bool winEffect = true;\n"
    "            if (gWindows) {\n"
    "                winEffect = (gMask & WINMASK_CLR) != 0u;\n"
    "                if ((gMask & (1u << uint(bg))) == 0u)\n"
    "                    continue;\n"
    "            }\n"
    "            if (gBlendMode != 0u && (gBldcnt & (1u << uint(bg))) != 0u && winEffect) {\n"
    "                if (gBlendMode == 1u) {\n"
    "                    vec4 b;\n"
    "                    if (selectTargetB(p, bg, numBgs, (gBldcnt & BLDCNT_TGT2_OBJ) != 0u, b))\n"
    "                        c = alphaBlend(c, b);\n"
    "                } else if (gBlendMode == 2u) {\n"
    "                    c = brightnessUp(c);\n"
    "                } else {\n"
    "                    c = brightnessDown(c);\n"
    "                }\n"
    "            }\n"
    "            color = c;\n"
    "        }\n"
    "        vec4 s = gSpriteTop[p];\n"
    "        if (drawn(s) && !(gWindows && (gMask & WINMASK_OBJ) == 0u))\n"
    "            color = s;\n"
    "    }\n"
    "    fragColor = vec4(to8bit(color.rgb), 1.0);\n"
    "}\n";

// Sharp bilinear: nearest-neighbour inside each source pixel, a one output
// pixel wide linear ramp at the edges, so non-integer scales stay crisp.
static const char sPresentFS[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uImage;\n"
    "uniform vec2 uSourceSize;\n"
    "uniform vec2 uScale;\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec2 texel = vUV * uSourceSize;\n"
    "    vec2 prescale = max(floor(uScale), vec2(1.0));\n"
    "    vec2 regionRange = 0.5 - 0.5 / prescale;\n"
    "    vec2 centerDist = fract(texel) - 0.5;\n"
    "    vec2 f = (centerDist - clamp(centerDist, -regionRange, regionRange)) * prescale + 0.5;\n"
    "    fragColor = texture(uImage, (floor(texel) + f) / uSourceSize);\n"
    "}\n";

// Overlay (menu) image, 240x160 RGBA, top row first
static const char sOverlayFS[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D uImage;\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    fragColor = texture(uImage, vec2(vUV.x, 1.0 - vUV.y));\n"
    "}\n";

// HD sprite quad: corners come in GBA pixels (y down), the atlas holds
// palette indices.
static const char sHdSpriteVS[] =
    "#version 300 es\n"
    "uniform vec2 uCorners[4];\n"   // TL, TR, BL, BR
    "uniform vec4 uFrameRect;\n"    // atlas x, y, width, height
    "out vec2 vAtlas;\n"
    "void main() {\n"
    "    int corner = gl_VertexID < 3 ? gl_VertexID : gl_VertexID - 2;\n" // 0 1 2, 1 2 3
    "    vec2 p = uCorners[corner];\n"
    "    vec2 local = vec2(float(corner & 1), float(corner >> 1));\n"
    "    vAtlas = uFrameRect.xy + local * uFrameRect.zw;\n"
    "    gl_Position = vec4(p.x / 120.0 - 1.0, 1.0 - p.y / 80.0, 0.0, 1.0);\n"
    "}\n";

static const char sHdSpriteFS[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp usampler2D;\n"
    "uniform usampler2D uAtlas;\n"
    "uniform sampler2D uPalette;\n"
    "uniform vec4 uFrameRect;\n"
    "uniform int uPaletteRow;\n"
    "uniform vec4 uTint;\n"
    "uniform int uMeta;\n"
    "in vec2 vAtlas;\n"
    "layout(location = 0) out vec4 outColor;\n"
    "layout(location = 1) out uint outMeta;\n"
    "void main() {\n"
    "    vec2 t = clamp(floor(vAtlas), uFrameRect.xy, uFrameRect.xy + uFrameRect.zw - 1.0);\n"
    "    uint index = texelFetch(uAtlas, ivec2(t), 0).r;\n"
    "    if (index == 0u)\n"
    "        discard;\n"
    "    vec3 c = texelFetch(uPalette, ivec2(int(index), uPaletteRow), 0).rgb;\n"
    "    outColor = vec4(mix(c, uTint.rgb, uTint.a), 1.0);\n"
    "    outMeta = uint(uMeta);\n"
    "}\n";

static GLuint sComposeProgram;
static GLuint sPresentProgram;
static GLuint sHdProgram;
static GLuint sOverlayProgram;
static GLuint sOverlayTexture;
static const uint8_t *sOverlay;
static uint32_t sOverlayVersion, sOverlayUploaded = (uint32_t)-1;
static GLuint sVao;
static GLuint sLayerTexture;    // R16UI array: one slice per PPU layer
static GLuint sObjIndexTexture; // R8UI array: OAM index of each sprite layer pixel
static GLuint sWinMaskTexture;  // R8UI
static GLuint sLineRegTexture;  // RGBA16UI, 2 texels per scanline
static GLuint sFrameTexture;    // RGBA8 composed frame
static GLuint sFrameFbo;
static GLuint sHdColorTexture;  // RGBA8 HD sprite layer
static GLuint sHdMetaTexture;   // R16UI: present, priority, OAM index, flags
static GLuint sHdFbo;
static int sScale = 1;          // compose scale the textures were made for
static int sWantedScale = 1;
static uint8_t *sReadBuffer;
static uint8_t *sRgbBuffer;

static GLint sComposeScaleLoc, sComposeHdLoc, sComposeField3DLoc;
static GLint sPresentScaleLoc, sPresentSourceLoc;
static GLint sHdCornersLoc, sHdFrameRectLoc, sHdPaletteRowLoc, sHdTintLoc, sHdMetaLoc;

static GLuint CompileShader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint ok = GL_FALSE;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "gl: shader compile failed:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint LinkProgram(const char *vsSource, const char *fsSource)
{
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vsSource);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fsSource);
    GLuint program;
    GLint ok = GL_FALSE;

    if (vs == 0 || fs == 0)
        return 0;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "gl: program link failed:\n%s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static GLuint CreateTexture(GLenum target, GLenum filter)
{
    GLuint texture;

    glGenTextures(1, &texture);
    glBindTexture(target, texture);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

// (Re)creates the compose-resolution render targets
static bool CreateScaledTargets(int scale)
{
    static const GLenum sHdBuffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    int width = DISPLAY_WIDTH * scale, height = DISPLAY_HEIGHT * scale;

    if (sFrameFbo != 0)
    {
        glDeleteFramebuffers(1, &sFrameFbo);
        glDeleteFramebuffers(1, &sHdFbo);
        glDeleteTextures(1, &sFrameTexture);
        glDeleteTextures(1, &sHdColorTexture);
        glDeleteTextures(1, &sHdMetaTexture);
    }

    sFrameTexture = CreateTexture(GL_TEXTURE_2D, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &sFrameFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, sFrameFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sFrameTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        fprintf(stderr, "gl: frame framebuffer incomplete\n");
        return false;
    }

    sHdColorTexture = CreateTexture(GL_TEXTURE_2D, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    sHdMetaTexture = CreateTexture(GL_TEXTURE_2D, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, width, height, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
    glGenFramebuffers(1, &sHdFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, sHdFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sHdColorTexture, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, sHdMetaTexture, 0);
    glDrawBuffers(2, sHdBuffers);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        fprintf(stderr, "gl: HD sprite framebuffer incomplete\n");
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    free(sReadBuffer);
    free(sRgbBuffer);
    sReadBuffer = malloc((size_t)width * height * 4);
    sRgbBuffer = malloc((size_t)width * height * 3);
    sScale = scale;
    return true;
}

bool GlRenderer_Init(void *(*getProc)(const char *name))
{
    const char *missing = NULL;

    if (!GlApi_Load(getProc, &missing))
    {
        fprintf(stderr, "gl: missing function %s\n", missing);
        return false;
    }
    printf("gl: %s / %s\n", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));

    sComposeProgram = LinkProgram(sFullscreenVS, sComposeFS);
    sPresentProgram = LinkProgram(sFullscreenVS, sPresentFS);
    sHdProgram = LinkProgram(sHdSpriteVS, sHdSpriteFS);
    sOverlayProgram = LinkProgram(sFullscreenVS, sOverlayFS);
    if (sComposeProgram == 0 || sPresentProgram == 0 || sHdProgram == 0 || sOverlayProgram == 0)
        return false;
    glUseProgram(sOverlayProgram);
    glUniform1i(glGetUniformLocation(sOverlayProgram, "uImage"), 0);

    glUseProgram(sComposeProgram);
    glUniform1i(glGetUniformLocation(sComposeProgram, "uLayers"), 0);
    glUniform1i(glGetUniformLocation(sComposeProgram, "uWinMask"), 1);
    glUniform1i(glGetUniformLocation(sComposeProgram, "uLineRegs"), 2);
    glUniform1i(glGetUniformLocation(sComposeProgram, "uObjIndex"), 3);
    glUniform1i(glGetUniformLocation(sComposeProgram, "uHdColor"), 4);
    glUniform1i(glGetUniformLocation(sComposeProgram, "uHdMeta"), 5);
    sComposeScaleLoc = glGetUniformLocation(sComposeProgram, "uScale");
    sComposeHdLoc = glGetUniformLocation(sComposeProgram, "uHdEnabled");
    glUniform1i(glGetUniformLocation(sComposeProgram, "uScene"), 6);
    sComposeField3DLoc = glGetUniformLocation(sComposeProgram, "uField3D");

    glUseProgram(sPresentProgram);
    glUniform1i(glGetUniformLocation(sPresentProgram, "uImage"), 0);
    sPresentSourceLoc = glGetUniformLocation(sPresentProgram, "uSourceSize");
    sPresentScaleLoc = glGetUniformLocation(sPresentProgram, "uScale");

    glUseProgram(sHdProgram);
    glUniform1i(glGetUniformLocation(sHdProgram, "uAtlas"), 0);
    glUniform1i(glGetUniformLocation(sHdProgram, "uPalette"), 1);
    sHdCornersLoc = glGetUniformLocation(sHdProgram, "uCorners");
    sHdFrameRectLoc = glGetUniformLocation(sHdProgram, "uFrameRect");
    sHdPaletteRowLoc = glGetUniformLocation(sHdProgram, "uPaletteRow");
    sHdTintLoc = glGetUniformLocation(sHdProgram, "uTint");
    sHdMetaLoc = glGetUniformLocation(sHdProgram, "uMeta");

    glGenVertexArrays(1, &sVao);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);

    sLayerTexture = CreateTexture(GL_TEXTURE_2D_ARRAY, GL_NEAREST);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R16UI, DISPLAY_WIDTH, DISPLAY_HEIGHT, PPU_LAYER_COUNT, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
    sObjIndexTexture = CreateTexture(GL_TEXTURE_2D_ARRAY, GL_NEAREST);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8UI, DISPLAY_WIDTH, DISPLAY_HEIGHT, 4, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);
    sOverlayTexture = CreateTexture(GL_TEXTURE_2D, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    sWinMaskTexture = CreateTexture(GL_TEXTURE_2D, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);
    sLineRegTexture = CreateTexture(GL_TEXTURE_2D, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16UI, PPU_LINE_REG_COUNT / 4, DISPLAY_HEIGHT, 0, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, NULL);

    if (!CreateScaledTargets(1))
        return false;
    if (!GlField3D_Init())
        return false;

    if (glGetError() != GL_NO_ERROR)
    {
        fprintf(stderr, "gl: error during setup\n");
        return false;
    }
    return true;
}

void GlRenderer_Shutdown(void)
{
    GlField3D_Shutdown();
    glDeleteFramebuffers(1, &sFrameFbo);
    glDeleteFramebuffers(1, &sHdFbo);
    glDeleteTextures(1, &sFrameTexture);
    glDeleteTextures(1, &sHdColorTexture);
    glDeleteTextures(1, &sHdMetaTexture);
    glDeleteTextures(1, &sLineRegTexture);
    glDeleteTextures(1, &sWinMaskTexture);
    glDeleteTextures(1, &sObjIndexTexture);
    glDeleteTextures(1, &sLayerTexture);
    glDeleteTextures(1, &sOverlayTexture);
    glDeleteProgram(sOverlayProgram);
    glDeleteVertexArrays(1, &sVao);
    glDeleteProgram(sHdProgram);
    glDeleteProgram(sPresentProgram);
    glDeleteProgram(sComposeProgram);
    free(sReadBuffer);
    free(sRgbBuffer);
}

void GlRenderer_SetComposeScale(int scale)
{
    if (scale < 1)
        scale = 1;
    if (scale > MAX_COMPOSE_SCALE)
        scale = MAX_COMPOSE_SCALE;
    sWantedScale = scale;
}

// Largest integer scale that fits; the presenter stretches the rest
int GlRenderer_ComposeScaleFor(int drawableWidth, int drawableHeight)
{
    int sx = drawableWidth / DISPLAY_WIDTH, sy = drawableHeight / DISPLAY_HEIGHT;
    return sx < sy ? sx : sy;
}

static void UploadAnim(struct HdAnim *anim)
{
    uint32_t palettes[2][256] = {{0}};
    int i;

    anim->gpuAtlas = CreateTexture(GL_TEXTURE_2D, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, anim->atlasWidth, anim->atlasHeight, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE, anim->atlas);
    for (i = 0; i < anim->paletteSize; i++)
    {
        palettes[0][i] = anim->palette[i];
        palettes[1][i] = anim->hasShinyPalette ? anim->shinyPalette[i] : anim->palette[i];
    }
    anim->gpuPalette = CreateTexture(GL_TEXTURE_2D, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, palettes);
}

static int CompareDrawOrder(const void *a, const void *b)
{
    const struct HdSpriteDraw *da = a, *db = b;
    // Back to front: higher priority value first, then higher OAM index
    if (da->priority != db->priority)
        return db->priority - da->priority;
    return db->oamIndex - da->oamIndex;
}

static void DrawHdSprites(const struct HdSpriteDraw *draws, int drawCount)
{
    static const GLuint sZeroMeta[4] = {0};
    static const GLfloat sZeroColor[4] = {0};
    struct HdSpriteDraw sorted[HD_SPRITE_MAX_DRAWS];
    int i;

    if (drawCount > HD_SPRITE_MAX_DRAWS)
        drawCount = HD_SPRITE_MAX_DRAWS;
    memcpy(sorted, draws, drawCount * sizeof(*draws));
    qsort(sorted, drawCount, sizeof(*sorted), CompareDrawOrder);

    glBindFramebuffer(GL_FRAMEBUFFER, sHdFbo);
    glViewport(0, 0, DISPLAY_WIDTH * sScale, DISPLAY_HEIGHT * sScale);
    glClearBufferfv(GL_COLOR, 0, sZeroColor);
    glClearBufferuiv(GL_COLOR, 1, sZeroMeta);
    glUseProgram(sHdProgram);
    glBindVertexArray(sVao);

    for (i = 0; i < drawCount; i++)
    {
        const struct HdSpriteDraw *draw = &sorted[i];
        struct HdAnim *anim = draw->anim;
        GLfloat corners[8];
        int c, column = draw->atlasFrame % anim->columns, row = draw->atlasFrame / anim->columns;
        GLuint meta = 0x8000 | (draw->priority << 7) | (draw->oamIndex & 0x7F)
                    | (draw->semiTransparent ? 0x200 : 0) | (draw->mosaic ? 0x400 : 0);

        if (anim->gpuAtlas == 0)
            UploadAnim(anim);
        for (c = 0; c < 4; c++)
        {
            corners[c * 2] = draw->corners[c][0];
            corners[c * 2 + 1] = draw->corners[c][1];
        }
        glUniform2fv(sHdCornersLoc, 4, corners);
        glUniform4f(sHdFrameRectLoc, column * anim->width, row * anim->height, anim->width, anim->height);
        glUniform1i(sHdPaletteRowLoc, draw->shiny ? 1 : 0);
        glUniform4f(sHdTintLoc, draw->tint[0], draw->tint[1], draw->tint[2], draw->tint[3]);
        glUniform1i(sHdMetaLoc, (GLint)meta);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, anim->gpuAtlas);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, anim->gpuPalette);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
}

void GlRenderer_ComposeFrame(const struct PpuFrameLayers *layers, const struct HdSpriteDraw *draws, int drawCount,
                             const struct Field3DFrame *field)
{
    GLuint scene = 0;

    if (sWantedScale != sScale)
        CreateScaledTargets(sWantedScale);

    glDisable(GL_BLEND);
    if (field != NULL && field->active)
        scene = GlField3D_Render(field, DISPLAY_WIDTH * sScale, DISPLAY_HEIGHT * sScale);
    if (drawCount > 0)
        DrawHdSprites(draws, drawCount);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, sLayerTexture);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, PPU_LAYER_COUNT,
                    GL_RED_INTEGER, GL_UNSIGNED_SHORT, layers->layers);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, sWinMaskTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GL_RED_INTEGER, GL_UNSIGNED_BYTE, layers->winMask);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, sLineRegTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, PPU_LINE_REG_COUNT / 4, DISPLAY_HEIGHT, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, layers->lineRegs);
    glActiveTexture(GL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, sObjIndexTexture);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, 4,
                    GL_RED_INTEGER, GL_UNSIGNED_BYTE, layers->objIndex);
    glActiveTexture(GL_TEXTURE0 + 4);
    glBindTexture(GL_TEXTURE_2D, sHdColorTexture);
    glActiveTexture(GL_TEXTURE0 + 5);
    glBindTexture(GL_TEXTURE_2D, sHdMetaTexture);
    glActiveTexture(GL_TEXTURE0 + 6);
    glBindTexture(GL_TEXTURE_2D, scene);

    glBindFramebuffer(GL_FRAMEBUFFER, sFrameFbo);
    glViewport(0, 0, DISPLAY_WIDTH * sScale, DISPLAY_HEIGHT * sScale);
    glUseProgram(sComposeProgram);
    glUniform1i(sComposeScaleLoc, sScale);
    glUniform1i(sComposeHdLoc, drawCount > 0);
    glUniform1i(sComposeField3DLoc, scene != 0);
    if (scene != 0)
    {
        bool wave = false;
        int line;
        for (line = 1; line < DISPLAY_HEIGHT && !wave; line++)
            wave = layers->lineRegs[line][PPU_LINE_BG2HOFS] != layers->lineRegs[0][PPU_LINE_BG2HOFS]
                || layers->lineRegs[line][PPU_LINE_BG2VOFS] != layers->lineRegs[0][PPU_LINE_BG2VOFS];
        glUniform1i(glGetUniformLocation(sComposeProgram, "uSceneWave"), wave);
        glUniform2f(glGetUniformLocation(sComposeProgram, "uSceneScroll"), field->scroll[0], field->scroll[1]);
    }
    glBindVertexArray(sVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (sOverlay != NULL)
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sOverlayTexture);
        if (sOverlayVersion != sOverlayUploaded)
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, sOverlay);
            sOverlayUploaded = sOverlayVersion;
        }
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(sOverlayProgram);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisable(GL_BLEND);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GlRenderer_SetOverlay(const uint8_t *rgba, uint32_t version)
{
    sOverlay = rgba;
    sOverlayVersion = version;
}

void GlRenderer_Present(int drawableWidth, int drawableHeight)
{
    // Largest 3:2 rectangle that fits, centered
    int width = drawableWidth;
    int height = drawableWidth * DISPLAY_HEIGHT / DISPLAY_WIDTH;
    float sourceWidth = DISPLAY_WIDTH * sScale, sourceHeight = DISPLAY_HEIGHT * sScale;

    if (height > drawableHeight)
    {
        height = drawableHeight;
        width = drawableHeight * DISPLAY_WIDTH / DISPLAY_HEIGHT;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, drawableWidth, drawableHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glViewport((drawableWidth - width) / 2, (drawableHeight - height) / 2, width, height);
    glUseProgram(sPresentProgram);
    glUniform2f(sPresentSourceLoc, sourceWidth, sourceHeight);
    glUniform2f(sPresentScaleLoc, width / sourceWidth, height / sourceHeight);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sFrameTexture);
    glBindVertexArray(sVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void ReadComposed(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, sFrameFbo);
    glReadPixels(0, 0, DISPLAY_WIDTH * sScale, DISPLAY_HEIGHT * sScale, GL_RGBA, GL_UNSIGNED_BYTE, sReadBuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GlRenderer_ReadFrame(uint16_t *pixels)
{
    int x, y, stride = DISPLAY_WIDTH * sScale * 4;

    ReadComposed();
    // GL rows start at the bottom
    for (y = 0; y < DISPLAY_HEIGHT; y++)
    {
        const uint8_t *row = &sReadBuffer[((DISPLAY_HEIGHT - 1 - y) * sScale + sScale / 2) * stride];
        for (x = 0; x < DISPLAY_WIDTH; x++)
        {
            const uint8_t *src = &row[(x * sScale + sScale / 2) * 4];
            pixels[y * DISPLAY_WIDTH + x] = (src[0] >> 3) | ((src[1] >> 3) << 5) | ((src[2] >> 3) << 10);
        }
    }
}

const uint8_t *GlRenderer_ReadFrameRgb(int *width, int *height)
{
    int w = DISPLAY_WIDTH * sScale, h = DISPLAY_HEIGHT * sScale, x, y;

    ReadComposed();
    for (y = 0; y < h; y++)
    {
        const uint8_t *src = &sReadBuffer[(h - 1 - y) * w * 4];
        uint8_t *dst = &sRgbBuffer[y * w * 3];
        for (x = 0; x < w; x++)
        {
            dst[x * 3 + 0] = src[x * 4 + 0];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 2];
        }
    }
    *width = w;
    *height = h;
    return sRgbBuffer;
}

#endif // PORTABLE
