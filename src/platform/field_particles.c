#ifdef PORTABLE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/field_particles.h"

// The part of the map the camera sees, around the screen center (cells)
#define VIEW_LEFT   9.5f
#define VIEW_NORTH  8.0f
#define VIEW_SOUTH  5.5f
#define CELL_SAMPLES 10   // cells looked at per update for water, steam and mist
#define MAX_GLINTS  30
#define GRAVITY     6.0f

enum
{
    KIND_MOTE,   // pollen, dust
    KIND_LEAF,
    KIND_EMBER,
    KIND_BUBBLE,
    KIND_GLINT,
    KIND_SPLASH,
    KIND_STEAM,
    KIND_MIST,
    KIND_BIT,    // thrown up by a footstep, falls back
    KIND_DUST,
};

struct Particle
{
    float pos[3], vel[3];
    float age, life;
    float size0, size1;
    float angle, spin;
    float phase;
    float floorY;      // height of the ground it falls onto
    uint8_t color[4];
    uint8_t shape, flags, kind;
};

struct Ambient
{
    int motes, leaves, embers, bubbles;
    uint8_t moteColor[3];
    uint8_t moteAlpha[2];   // min, max
    float moteSize[2];
    float moteDrift;        // speed of the random drift
    float glintChance;      // per water cell looked at
};

static const struct Ambient sAmbient[PARTICLE_THEME_COUNT] = {
    [PARTICLE_THEME_NONE] = {0},
    [PARTICLE_THEME_MEADOW] = {.motes = 34, .moteColor = {255, 240, 190}, .moteAlpha = {170, 255},
                               .moteSize = {0.128f, 0.208f}, .moteDrift = 0.25f, .glintChance = 0.35f},
    [PARTICLE_THEME_FOREST] = {.motes = 18, .leaves = 14, .moteColor = {235, 255, 180}, .moteAlpha = {160, 240},
                               .moteSize = {0.128f, 0.192f}, .moteDrift = 0.2f, .glintChance = 0.3f},
    [PARTICLE_THEME_CAVE] = {.motes = 45, .moteColor = {235, 215, 180}, .moteAlpha = {90, 160},
                             .moteSize = {0.096f, 0.160f}, .moteDrift = 0.12f},
    [PARTICLE_THEME_INDOOR] = {.motes = 14, .moteColor = {255, 245, 220}, .moteAlpha = {100, 170},
                               .moteSize = {0.096f, 0.144f}, .moteDrift = 0.1f},
    [PARTICLE_THEME_EMBERS] = {.motes = 10, .embers = 45, .moteColor = {255, 200, 150}, .moteAlpha = {100, 170},
                               .moteSize = {0.096f, 0.160f}, .moteDrift = 0.2f},
    [PARTICLE_THEME_UNDERWATER] = {.bubbles = 40, .motes = 20, .moteColor = {200, 235, 255}, .moteAlpha = {80, 150},
                                   .moteSize = {0.096f, 0.160f}, .moteDrift = 0.15f},
    [PARTICLE_THEME_OCEAN] = {.motes = 14, .moteColor = {255, 250, 220}, .moteAlpha = {170, 255},
                              .moteSize = {0.128f, 0.192f}, .moteDrift = 0.3f, .glintChance = 0.6f},
};

static struct Particle sParticles[FIELD3D_MAX_PARTICLES];
static int sCount;
static struct Field3DParticle sOut[FIELD3D_MAX_PARTICLES];
static uint32_t sRng = 0x9E3779B9u;
static float sTime;
static bool sFilled;
static bool sDebug; // PE_DEBUG_PARTICLES: counts in the log once a second

static float Random(void)
{
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return (sRng >> 8) * (1.0f / 16777216.0f);
}

static float Range(float a, float b)
{
    return a + (b - a) * Random();
}

static float Clamp01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

void Particles_Reset(void)
{
    sCount = 0;
    sFilled = false;
    sDebug = getenv("PE_DEBUG_PARTICLES") != NULL;
}

static struct Particle *Spawn(int kind)
{
    struct Particle *p;

    if (sCount >= FIELD3D_MAX_PARTICLES)
        return NULL;
    p = &sParticles[sCount++];
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->phase = Range(0, 6.2831853f);
    return p;
}

// World position of a point in a grid cell, h blocks above its ground
static void CellPoint(const struct ParticleWorld *w, float gx, float gy, float h, float out[3])
{
    float ground = w->surface((int)floorf(gx), (int)floorf(gy));
    out[0] = gx;
    out[1] = ground + h;
    out[2] = gy + ground;
}

static void RandomViewCell(const struct ParticleWorld *w, float *gx, float *gy)
{
    *gx = w->center[0] + Range(-VIEW_LEFT, VIEW_LEFT);
    *gy = w->center[1] + Range(-VIEW_NORTH, VIEW_SOUTH);
}

static int CountKind(int kind)
{
    int i, n = 0;

    for (i = 0; i < sCount; i++)
        n += sParticles[i].kind == kind;
    return n;
}

static void SetColor(struct Particle *p, int r, int g, int b, int a)
{
    p->color[0] = r;
    p->color[1] = g;
    p->color[2] = b;
    p->color[3] = a;
}

// --- spawners --------------------------------------------------------------

static void SpawnMote(const struct ParticleWorld *w, const struct Ambient *a, bool anyAge)
{
    struct Particle *p = Spawn(KIND_MOTE);
    float gx, gy;

    if (p == NULL)
        return;
    RandomViewCell(w, &gx, &gy);
    CellPoint(w, gx, gy, Range(0.3f, 2.4f), p->pos);
    p->vel[0] = Range(-1, 1) * a->moteDrift;
    p->vel[1] = Range(-0.3f, 0.3f) * a->moteDrift;
    p->vel[2] = Range(-1, 1) * a->moteDrift;
    p->life = Range(4.0f, 9.0f);
    p->age = anyAge ? Range(0, p->life) : 0;
    p->size0 = p->size1 = Range(a->moteSize[0], a->moteSize[1]);
    SetColor(p, a->moteColor[0], a->moteColor[1], a->moteColor[2], (int)Range(a->moteAlpha[0], a->moteAlpha[1]));
    p->shape = FIELD3D_PARTICLE_DOT;
    p->flags = FIELD3D_PARTICLE_GLOW;
}

static void SpawnLeaf(const struct ParticleWorld *w, bool anyAge)
{
    static const uint8_t sColors[4][3] = {{88, 160, 64}, {132, 186, 76}, {190, 176, 72}, {112, 150, 56}};
    struct Particle *p = Spawn(KIND_LEAF);
    const uint8_t *c;
    float gx, gy;

    if (p == NULL)
        return;
    RandomViewCell(w, &gx, &gy);
    gy -= 1.5f; // they fall towards the camera's view
    CellPoint(w, gx, gy, Range(2.0f, 3.5f), p->pos);
    p->floorY = w->surface((int)floorf(gx), (int)floorf(gy)) + 0.02f;
    p->vel[1] = -Range(0.25f, 0.45f);
    p->life = Range(7.0f, 11.0f);
    p->age = anyAge ? Range(0, p->life * 0.6f) : 0;
    if (anyAge)
        p->pos[1] += p->vel[1] * p->age;
    p->size0 = p->size1 = Range(0.1f, 0.14f);
    p->angle = Range(0, 6.28f);
    p->spin = Range(-2.5f, 2.5f);
    c = sColors[(int)Range(0, 3.99f)];
    SetColor(p, c[0], c[1], c[2], 235);
    p->shape = FIELD3D_PARTICLE_LEAF;
}

static void SpawnEmber(const struct ParticleWorld *w, bool anyAge)
{
    struct Particle *p = Spawn(KIND_EMBER);
    float gx, gy;

    if (p == NULL)
        return;
    RandomViewCell(w, &gx, &gy);
    CellPoint(w, gx, gy, Range(0.0f, 0.6f), p->pos);
    p->vel[0] = Range(-0.15f, 0.15f);
    p->vel[1] = Range(0.5f, 1.1f);
    p->vel[2] = Range(-0.1f, 0.1f);
    p->life = Range(2.0f, 4.0f);
    p->age = anyAge ? Range(0, p->life) : 0;
    if (anyAge)
        p->pos[1] += p->vel[1] * p->age;
    p->size0 = Range(0.13f, 0.2f);
    p->size1 = p->size0 * 0.6f;
    SetColor(p, 255, 210, 100, 255);
    p->shape = FIELD3D_PARTICLE_DOT;
    p->flags = FIELD3D_PARTICLE_GLOW;
}

static void SpawnBubble(const struct ParticleWorld *w, bool anyAge)
{
    struct Particle *p = Spawn(KIND_BUBBLE);
    float gx, gy;

    if (p == NULL)
        return;
    RandomViewCell(w, &gx, &gy);
    CellPoint(w, gx, gy, Range(0.0f, 0.5f), p->pos);
    p->vel[1] = Range(0.4f, 0.8f);
    p->life = Range(3.0f, 5.0f);
    p->age = anyAge ? Range(0, p->life) : 0;
    if (anyAge)
        p->pos[1] += p->vel[1] * p->age;
    p->size0 = Range(0.07f, 0.12f);
    p->size1 = p->size0 * 1.4f;
    SetColor(p, 220, 245, 255, 230);
    p->shape = FIELD3D_PARTICLE_BUBBLE;
}

static void SpawnGlint(const struct ParticleWorld *w, float gx, float gy)
{
    struct Particle *p = Spawn(KIND_GLINT);

    if (p == NULL)
        return;
    CellPoint(w, gx, gy, 0.03f, p->pos);
    p->life = Range(0.35f, 0.7f);
    p->size0 = p->size1 = Range(0.14f, 0.24f);
    p->angle = Random() < 0.5f ? 0.0f : 0.785f;
    SetColor(p, 255, 255, 250, 255);
    p->shape = FIELD3D_PARTICLE_SPARKLE;
    p->flags = FIELD3D_PARTICLE_GLOW;
}

static void SpawnSteam(const struct ParticleWorld *w, float gx, float gy, bool mist)
{
    struct Particle *p = Spawn(mist ? KIND_MIST : KIND_STEAM);

    if (p == NULL)
        return;
    CellPoint(w, gx, gy, mist ? Range(0.0f, 0.4f) : 0.05f, p->pos);
    p->vel[0] = Range(-0.15f, 0.15f) * (mist ? 3 : 1);
    p->vel[1] = mist ? Range(0.1f, 0.3f) : Range(0.25f, 0.45f);
    p->vel[2] = mist ? Range(0.1f, 0.5f) : Range(-0.1f, 0.1f);
    p->life = mist ? Range(0.8f, 1.4f) : Range(1.8f, 2.8f);
    p->size0 = Range(0.16f, 0.26f);
    p->size1 = p->size0 * (mist ? 2.2f : 2.8f);
    SetColor(p, 255, 255, 255, 235);
    p->shape = FIELD3D_PARTICLE_PUFF;
}

static void SpawnSplash(const struct ParticleWorld *w, float gx, float gy, bool big)
{
    struct Particle *p = Spawn(KIND_SPLASH);

    if (p == NULL)
        return;
    CellPoint(w, gx, gy, 0.02f, p->pos);
    p->life = big ? 0.6f : Range(0.35f, 0.5f);
    p->size0 = 0.05f;
    p->size1 = big ? 0.55f : Range(0.25f, 0.4f);
    SetColor(p, 240, 248, 255, 255);
    p->shape = FIELD3D_PARTICLE_RING;
    p->flags = FIELD3D_PARTICLE_FLAT;
}

// Thrown up by a footstep: leaf bits, ash flakes, drops
static void SpawnBits(const struct ParticleWorld *w, int x, int y, int count, const uint8_t (*colors)[3], int colorCount,
                      int shape, float size)
{
    int i;

    for (i = 0; i < count; i++)
    {
        struct Particle *p = Spawn(KIND_BIT);
        const uint8_t *c;
        if (p == NULL)
            return;
        CellPoint(w, x + Range(0.25f, 0.75f), y + Range(0.6f, 1.0f), 0.35f, p->pos);
        p->floorY = w->surface(x, y) + 0.01f;
        p->vel[0] = Range(-1.0f, 1.0f);
        p->vel[1] = Range(2.4f, 3.4f);
        p->vel[2] = Range(-0.2f, 0.8f);
        p->life = Range(0.7f, 1.0f);
        p->size0 = p->size1 = size * Range(0.8f, 1.2f);
        p->angle = Range(0, 6.28f);
        p->spin = Range(-9, 9);
        c = colors[(int)Range(0, colorCount - 0.01f)];
        SetColor(p, c[0], c[1], c[2], 240);
        p->shape = shape;
    }
}

static void SpawnDust(const struct ParticleWorld *w, int x, int y, int count, int r, int g, int b)
{
    int i;

    for (i = 0; i < count; i++)
    {
        struct Particle *p = Spawn(KIND_DUST);
        if (p == NULL)
            return;
        CellPoint(w, x + Range(0.2f, 0.8f), y + Range(0.5f, 0.95f), 0.05f, p->pos);
        p->vel[0] = Range(-0.4f, 0.4f);
        p->vel[1] = Range(0.15f, 0.35f);
        p->life = Range(0.45f, 0.7f);
        p->size0 = Range(0.07f, 0.1f);
        p->size1 = p->size0 * 2.5f;
        SetColor(p, r, g, b, 150);
        p->shape = FIELD3D_PARTICLE_PUFF;
    }
}

static void SpawnFromSteps(const struct ParticleWorld *w)
{
    static const uint8_t sGrass[3][3] = {{150, 214, 96}, {190, 230, 120}, {110, 190, 80}};
    static const uint8_t sAsh[2][3] = {{150, 150, 150}, {190, 190, 186}};
    static const uint8_t sDrops[1][3] = {{220, 240, 255}};
    int i;

    for (i = 0; i < w->stepCount; i++)
    {
        int x = w->steps[i][0], y = w->steps[i][1];
        uint32_t flags = w->cellFlags(x, y);

        if (flags & PARTICLE_CELL_GRASS)
            SpawnBits(w, x, y, 9, sGrass, 3, FIELD3D_PARTICLE_LEAF, 0.07f);
        else if (flags & PARTICLE_CELL_ASHGRASS)
        {
            SpawnBits(w, x, y, 6, sAsh, 2, FIELD3D_PARTICLE_DOT, 0.03f);
            SpawnDust(w, x, y, 2, 170, 170, 165);
        }
        else if (flags & PARTICLE_CELL_SAND)
            SpawnDust(w, x, y, 3, 222, 204, 160);
        else if (flags & PARTICLE_CELL_WATER)
        {
            SpawnSplash(w, x + 0.5f, y + 0.5f, true);
            SpawnBits(w, x, y, 4, sDrops, 1, FIELD3D_PARTICLE_DOT, 0.03f);
        }
    }
}

// --- update ----------------------------------------------------------------

static void Simulate(struct Particle *p, const struct ParticleWorld *w, float dt)
{
    float t;

    p->age += dt;
    t = p->age / p->life;
    switch (p->kind)
    {
    case KIND_MOTE:
        p->vel[0] += Range(-0.3f, 0.3f) * dt;
        p->vel[2] += Range(-0.3f, 0.3f) * dt;
        p->pos[0] += (p->vel[0] + w->wind * 0.25f) * dt;
        p->pos[1] += (p->vel[1] + sinf(sTime * 1.3f + p->phase) * 0.08f) * dt;
        p->pos[2] += p->vel[2] * dt;
        break;
    case KIND_LEAF:
        if (p->pos[1] > p->floorY)
        {
            p->pos[0] += (sinf(p->age * 1.8f + p->phase) * 0.55f + w->wind * 0.5f) * dt;
            p->pos[1] += p->vel[1] * dt;
            p->pos[2] += cosf(p->age * 1.3f + p->phase) * 0.15f * dt;
            p->angle += p->spin * dt;
            if (p->pos[1] <= p->floorY)
            {
                p->pos[1] = p->floorY;
                p->life = p->age + 1.5f; // lies there a moment, then fades
                p->flags |= FIELD3D_PARTICLE_FLAT;
            }
        }
        break;
    case KIND_EMBER:
        p->pos[0] += (p->vel[0] + sinf(p->age * 3.0f + p->phase) * 0.2f + w->wind * 0.2f) * dt;
        p->pos[1] += p->vel[1] * dt;
        p->pos[2] += p->vel[2] * dt;
        // yellow -> orange -> dark red
        p->color[1] = (uint8_t)(200 - 150 * t);
        p->color[2] = (uint8_t)(90 - 80 * t);
        break;
    case KIND_BUBBLE:
        p->pos[0] += sinf(p->age * 2.5f + p->phase) * 0.15f * dt;
        p->pos[1] += p->vel[1] * dt;
        break;
    case KIND_STEAM:
    case KIND_MIST:
    case KIND_DUST:
        p->pos[0] += (p->vel[0] + w->wind * 0.15f) * dt;
        p->pos[1] += p->vel[1] * dt;
        p->pos[2] += p->vel[2] * dt;
        p->vel[0] *= 1.0f - 1.5f * dt;
        p->vel[2] *= 1.0f - 1.5f * dt;
        break;
    case KIND_BIT:
        if (p->pos[1] > p->floorY || p->vel[1] > 0)
        {
            p->vel[1] -= GRAVITY * dt;
            p->pos[0] += p->vel[0] * dt;
            p->pos[1] += p->vel[1] * dt;
            p->pos[2] += p->vel[2] * dt;
            p->angle += p->spin * dt;
            if (p->pos[1] < p->floorY)
                p->pos[1] = p->floorY;
        }
        break;
    }
}

// Opacity over a particle's life
static float Envelope(const struct Particle *p)
{
    float t = p->age / p->life;

    switch (p->kind)
    {
    case KIND_MOTE:
        return Clamp01(p->age / 1.2f) * Clamp01((p->life - p->age) / 1.2f) * (0.7f + 0.3f * sinf(sTime * 2.7f + p->phase));
    case KIND_LEAF:
        return Clamp01(p->age / 0.8f) * Clamp01((p->life - p->age) / 1.0f);
    case KIND_EMBER:
        return Clamp01(p->age / 0.3f) * (1.0f - t) * (0.75f + 0.25f * sinf(sTime * 17.0f + p->phase * 5.0f));
    case KIND_BUBBLE:
        return Clamp01(p->age / 0.4f) * Clamp01((p->life - p->age) / 0.5f);
    case KIND_GLINT:
        return sinf(3.14159f * t);
    case KIND_SPLASH:
        return 1.0f - t;
    case KIND_STEAM:
    case KIND_MIST:
        return Clamp01(p->age / 0.3f) * (1.0f - t);
    case KIND_DUST:
        return Clamp01(p->age / 0.25f) * (1.0f - t) * (1.0f - t);
    case KIND_BIT:
        return Clamp01((p->life - p->age) / 0.25f);
    }
    return 1.0f;
}

static bool OutOfView(const struct Particle *p, const struct ParticleWorld *w)
{
    float dx = p->pos[0] - w->center[0];
    float dz = p->pos[2] - p->pos[1] - w->center[1]; // undo the level shift, roughly

    return dx < -VIEW_LEFT - 2 || dx > VIEW_LEFT + 2 || dz < -VIEW_NORTH - 3 || dz > VIEW_SOUTH + 3;
}

int Particles_Update(const struct ParticleWorld *w, float dt, const struct Field3DParticle **out)
{
    const struct Ambient *a = &sAmbient[w->theme >= 0 && w->theme < PARTICLE_THEME_COUNT ? w->theme : 0];
    bool outdoor = w->theme == PARTICLE_THEME_MEADOW || w->theme == PARTICLE_THEME_FOREST || w->theme == PARTICLE_THEME_OCEAN;
    int motes = a->motes, leaves = a->leaves, i, n;
    float frames = dt * 60.0f;

    if (dt > 0.1f)
        dt = 0.1f;
    sTime += dt;
    if (outdoor && (w->rain || w->haze))
        motes = leaves = 0;

    // keep the ambient ones topped up (filled at once on a new map)
    for (i = CountKind(KIND_MOTE); i < motes; i++)
        SpawnMote(w, a, !sFilled);
    for (i = CountKind(KIND_LEAF); i < leaves; i++)
        SpawnLeaf(w, !sFilled);
    for (i = CountKind(KIND_EMBER); i < a->embers; i++)
        SpawnEmber(w, !sFilled);
    for (i = CountKind(KIND_BUBBLE); i < a->bubbles; i++)
        SpawnBubble(w, !sFilled);
    sFilled = true;

    // cells around the screen: glints on water, steam, waterfall mist
    for (i = 0; i < CELL_SAMPLES && frames > 0; i++)
    {
        float gx, gy;
        uint32_t flags;
        if (Random() > frames)
            continue;
        RandomViewCell(w, &gx, &gy);
        flags = w->cellFlags((int)floorf(gx), (int)floorf(gy));
        if ((flags & PARTICLE_CELL_WATER) && w->sunny && outdoor && Random() < a->glintChance
         && CountKind(KIND_GLINT) < MAX_GLINTS)
            SpawnGlint(w, gx, gy);
        if (flags & PARTICLE_CELL_HOTSPRING)
            SpawnSteam(w, gx, gy, false);
        if ((flags & PARTICLE_CELL_WATERFALL) && Random() < 0.8f)
            SpawnSteam(w, gx, gy, true);
    }

    // rain: rings where the drops land
    if (w->rain && outdoor)
    {
        float rate = (w->heavyRain ? 200.0f : 120.0f) * dt;
        while (rate > 0)
        {
            float gx, gy;
            if (Random() < rate)
            {
                RandomViewCell(w, &gx, &gy);
                if (!(w->cellFlags((int)floorf(gx), (int)floorf(gy)) & PARTICLE_CELL_BLOCKED))
                    SpawnSplash(w, gx, gy, false);
            }
            rate -= 1.0f;
        }
    }

    SpawnFromSteps(w);

    // move, drop the old ones (and ambient ones the camera left behind)
    for (i = 0; i < sCount;)
    {
        struct Particle *p = &sParticles[i];
        Simulate(p, w, dt);
        if (p->age >= p->life || OutOfView(p, w))
            sParticles[i] = sParticles[--sCount];
        else
            i++;
    }

    for (i = 0, n = 0; i < sCount; i++)
    {
        const struct Particle *p = &sParticles[i];
        struct Field3DParticle *o = &sOut[n];
        float t = p->age / p->life, alpha = Envelope(p);
        if (alpha <= 0.01f)
            continue;
        memcpy(o->pos, p->pos, sizeof(o->pos));
        o->size = p->size0 + (p->size1 - p->size0) * t;
        if (p->kind == KIND_GLINT)
            o->size *= 0.6f + 0.4f * sinf(3.14159f * t);
        o->angle = p->angle;
        o->color[0] = p->color[0];
        o->color[1] = p->color[1];
        o->color[2] = p->color[2];
        o->color[3] = (uint8_t)(p->color[3] * Clamp01(alpha));
        o->shape = p->shape;
        o->flags = p->flags;
        n++;
    }
    *out = sOut;
    if (sDebug && (int)(sTime * 60) % 60 == 0)
    {
        int counts[10] = {0};
        for (i = 0; i < sCount; i++)
            counts[sParticles[i].kind]++;
        printf("particle kinds: mote %d leaf %d ember %d bubble %d glint %d splash %d steam %d mist %d bit %d dust %d"
               " rain %d sunny %d\n", counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6],
               counts[7], counts[8], counts[9], w->rain, w->sunny);
    }
    return n;
}

#endif // PORTABLE
