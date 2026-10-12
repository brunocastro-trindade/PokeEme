#ifndef GUARD_PLATFORM_FIELD_PARTICLES_H
#define GUARD_PLATFORM_FIELD_PARTICLES_H

#include <stdbool.h>
#include <stdint.h>
#include "platform/field3d.h"

// Particle effects of the 3D field: ambient ones picked by the map's theme
// (pollen, leaves, cave dust, embers, bubbles), sun glints on water, steam
// over hot springs, mist under waterfalls, rain splashes, and bits thrown up
// by footsteps in grass, ash and sand. Plain simulation, no GL and no game
// state: field3d.c describes the world through ParticleWorld.

// Themes, as written in the field tables (hd2d/tools/mapconv/placement.py)
enum
{
    PARTICLE_THEME_NONE,
    PARTICLE_THEME_MEADOW,     // floating pollen
    PARTICLE_THEME_FOREST,     // falling leaves and pollen
    PARTICLE_THEME_CAVE,       // dust in the air
    PARTICLE_THEME_INDOOR,     // a little dust in the light
    PARTICLE_THEME_EMBERS,     // embers rising (volcano)
    PARTICLE_THEME_UNDERWATER, // bubbles
    PARTICLE_THEME_OCEAN,      // pollen, more glints on the water
    PARTICLE_THEME_COUNT,
};

// What a map cell is, for spawning
#define PARTICLE_CELL_WATER     0x01 // water surface (glints, splashes)
#define PARTICLE_CELL_HOTSPRING 0x02
#define PARTICLE_CELL_WATERFALL 0x04
#define PARTICLE_CELL_GRASS     0x08
#define PARTICLE_CELL_ASHGRASS  0x10
#define PARTICLE_CELL_SAND      0x20
#define PARTICLE_CELL_BLOCKED   0x40

#define PARTICLE_MAX_STEPS 16

struct ParticleWorld
{
    int theme;
    float center[2];          // grid cell at the middle of the screen (x, y)
    bool sunny;               // sun out: glints on the water
    bool rain, heavyRain;
    bool haze;                // fog, ash or sand in the air: no floating pollen
    float wind;               // -1..1, the same gusts that move the grass
    uint32_t (*cellFlags)(int x, int y);  // PARTICLE_CELL_* of a grid cell
    float (*surface)(int x, int y);       // ground height of a grid cell (blocks)
    int stepCount;                        // cells people stepped into since the last update
    int16_t steps[PARTICLE_MAX_STEPS][2];
};

// Drops every particle (new map, effect turned off)
void Particles_Reset(void);

// Advances dt seconds; returns the particles to draw
int Particles_Update(const struct ParticleWorld *world, float dt, const struct Field3DParticle **out);

#endif // GUARD_PLATFORM_FIELD_PARTICLES_H
