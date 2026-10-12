#ifndef GUARD_PLATFORM_FIELD3D_H
#define GUARD_PLATFORM_FIELD3D_H

#include <stdbool.h>
#include <stdint.h>

// HD-2D field: builds a 3D scene of the overworld from the game's map grid,
// the placement tables made by hd2d/tools/mapconv and the sprites in OAM.
// No GL here; gl_field3d.c draws what Field3D_Prepare describes.
//
// World units: 1 = one 16x16 block. X grows east, Y up, Z south. Block
// (x, y) of gBackupMapLayout covers X x..x+1, Z y..y+1.

#define FIELD3D_FLAG_CUTOUT  0x01 // drop pixels equal to the floor under the cell
#define FIELD3D_FLAG_UPRIGHT 0x02 // stands up facing south (else lies flat facing up)
#define FIELD3D_FLAG_REPEAT  0x04 // the image repeats past 16 pixels (else it stretches to the quad)
#define FIELD3D_FLAG_NOSHADOW 0x08 // casts no shadow (ground lying on its base: nothing is under it)

struct Field3DVertex
{
    float pos[3];
    float uv[2];       // pixel inside the 16x16 metatile
    uint16_t metatile; // 0-1023
    uint16_t floor;    // metatile compared against for cutouts
    uint8_t layer;     // 0 bottom, 1 top
    uint8_t flags;
    uint8_t sway;      // how far wind and passers-by move this vertex (grass tips), 0-255
    uint8_t unused;
    uint16_t cutMask;  // palette colors cut out of a card (bit n = color n)
};

#define FIELD3D_BB_HFLIP  0x01
#define FIELD3D_BB_VFLIP  0x02
#define FIELD3D_BB_8BPP   0x04
#define FIELD3D_BB_FLAT   0x08 // lies on the ground (shadows, footprints, ripples, reflections); casts no shadow
#define FIELD3D_BB_PAPER  0x10 // paper style: a white die-cut rim one pixel around the figure
#define FIELD3D_BB_BASE   0x20 // paper style: the stand under a figure (drawn by the shader, width x height pixels)

// Sides of a paper figure's piece where the rim may grow past the piece
// (not where another piece of the same sprite continues it)
#define FIELD3D_EDGE_LEFT  0x01
#define FIELD3D_EDGE_RIGHT 0x02
#define FIELD3D_EDGE_TOP   0x04

// One OAM entry of a world sprite, drawn standing up
struct Field3DBillboard
{
    float x0, x1;   // world X span
    float y0, y1;   // world height span (bottom, top); flat ones: height in y0
    float z;        // world Z of the plane; flat ones: Z of the top edge (north), y1 = Z of the bottom edge
    uint16_t tileNum;
    uint8_t width, height; // pixels
    uint8_t cutTop;        // pixels hidden at the top (people under a bridge)
    uint8_t palette;
    uint8_t flags;
    uint8_t oamIndex;
    uint8_t edges;         // FIELD3D_EDGE_*, paper figures
    float tilt;            // radians, rocking in its own plane around (pivotX, pivotY) (paper figures walking)
    float pivotX, pivotY;
};

#define FIELD3D_MAX_DOORS      8
#define FIELD3D_METATILE_ROWS  (1024 + FIELD3D_MAX_DOORS) // map metatiles, then door animation frames
#define FIELD3D_MAX_BILLBOARDS 128
#define FIELD3D_MAX_ACTORS     16
#define FIELD3D_MAX_PARTICLES  384

// Particle shapes (drawn by the shader, no textures)
enum
{
    FIELD3D_PARTICLE_DOT,     // soft glowing dot
    FIELD3D_PARTICLE_SPARKLE, // four-pointed star
    FIELD3D_PARTICLE_LEAF,
    FIELD3D_PARTICLE_RING,    // thin circle (ripples)
    FIELD3D_PARTICLE_BUBBLE,
    FIELD3D_PARTICLE_PUFF,    // soft cloud (steam, dust)
    FIELD3D_PARTICLE_CONTACT, // soft ellipse, half as deep as wide (shadow under the feet)
};

#define FIELD3D_PARTICLE_FLAT 0x01 // lies on the ground instead of facing the camera
#define FIELD3D_PARTICLE_GLOW 0x02 // adds light (else covers what is behind)

struct Field3DParticle
{
    float pos[3];      // world position of the center
    float size;        // half width, blocks
    float angle;       // rotation, radians
    uint8_t color[4];  // rgb, a = opacity
    uint8_t shape, flags;
};

// Light setups, picked from the map type
enum
{
    FIELD3D_LIGHT_OUTDOOR,
    FIELD3D_LIGHT_INDOOR,
    FIELD3D_LIGHT_CAVE,
    FIELD3D_LIGHT_UNDERWATER,
};

struct Field3DFrame
{
    bool active;
    float target[3];                 // world point at the center of the GBA screen
    uint16_t scroll[2];              // map BG scroll (HOFS, VOFS) the scene matches; per-line
                                     // differences (battle transitions) shift the scene
    float mapMin[2], mapMax[2];      // world X, Z covered by the layout (without the border)
    float time;                      // seconds since start, for the wind
    int lighting;                    // FIELD3D_LIGHT_*
    bool openAir;                    // town, route or sea: particles, blur, glow and haze allowed
    float dim;                       // 0 clear, 1 heavy rain or fog (darker, flatter light)
    float hour;                      // time of day in the game (0-24) for the outdoor light; < 0: always day
    float fade[2];                   // screen fades the game does with the palettes: brightness
                                     // (1 normal, 0 black) and whiteness (0 normal, 1 white)
    float actors[FIELD3D_MAX_ACTORS][2]; // feet X, Z of people (the grass bends around them)
    int actorCount;
    const struct Field3DParticle *particles;
    int particleCount;
    const struct Field3DVertex *vertices;
    int vertexCount;                 // multiple of 6 (two triangles per quad)
    uint32_t meshVersion;            // changes whenever the mesh is rebuilt
    uint32_t patchVersion;           // changes when vertices patchFirst.. (patchCount) changed in place
    int patchFirst, patchCount;
    const uint16_t *metatileEntries; // FIELD3D_METATILE_ROWS x 8 tile entries
    uint32_t metatileVersion;
    struct Field3DBillboard billboards[FIELD3D_MAX_BILLBOARDS];
    int billboardCount;
};

// Checks the tables in dataDir: FIELD3D_DATA_*
enum { FIELD3D_DATA_OK, FIELD3D_DATA_MISSING, FIELD3D_DATA_OLD };
int Field3D_Init(const char *dataDir);
void Field3D_SetEnabled(bool enabled);
bool Field3D_IsEnabled(void);

// Describes this frame's 3D field and marks in skipOam the OAM entries it
// draws (so the PPU leaves them out). Returns false when the field is not
// on screen or has no data; then the 2D image is used as is.
// On PC the layout's table is reloaded when its file changes (map editor).
bool Field3D_Prepare(struct Field3DFrame *frame, uint8_t *skipOam);

#endif // GUARD_PLATFORM_FIELD3D_H
