#ifdef PORTABLE
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "global.h"
#include "field_camera.h"
#include "field_weather.h"
#include "fieldmap.h"
#include "main.h"
#include "metatile_behavior.h"
#include "field_effect_helpers.h"
#include "field_player_avatar.h"
#include "constants/event_objects.h"
#include "constants/field_effects.h"
#include "event_object_movement.h"
#include "overworld.h"
#include "palette.h"
#include "platform.h"
#include "sprite.h"
#include "platform/field3d.h"
#include "platform/field_particles.h"
#include "platform/harness.h"
#include "platform/hd_settings.h"
#include "platform/ppu_layers.h"
#include "constants/map_types.h"
#include "constants/weather.h"

// Placement kinds, as written by hd2d/tools/mapconv/placement.py
enum { KIND_NONE, KIND_GROUND, KIND_CARD, KIND_WALL, KIND_ROOF, KIND_WATER, KIND_GRASS, KIND_FACE, KIND_TOP };

#define RAISED        0.008f // a flat this far over the ground under it is a roof or rock top (casts a shadow)
#define WATER_DEPTH   0.12f
#define SPRITE_BIAS   0.0004f // lower OAM indices win ties, as on the GBA
#define VIEW_MARGIN   12      // blocks of border drawn around the map grid (the camera sees past the GBA screen)
#define GRASS_HEIGHT  0.7f    // standing grass: two rows of blades per cell, this tall
#define RELOAD_FRAMES 30      // how often the table file is checked for changes (PC)
#define TABLE_CACHE   8       // the map, its neighbors (4 at most) and the maps just left
#define MAX_NEIGHBORS 8
#define MAX_CONTACTS  32
#define CONTACT_ALPHA 0.6f    // darkness of the soft shadow under people's feet
#define STEP_TILT     0.09f   // paper figures lean this far (radians) halfway through a step

struct FieldTable
{
    int layoutId;  // -1: empty slot
    bool loaded;   // false: the layout has no table (looked for once)
    int64_t time;  // file time when it was read
    uint32_t lastUse;
    int width, height;
    uint16_t defaultFloor;
    uint16_t particleTheme;
    uint8_t placement[1024][2];
    uint16_t cutMask[1024][2];
    uint16_t *floor;
    uint8_t *level; // quarter blocks
    uint8_t *base;  // ground under the cell, quarter blocks (below stacked walls)
    uint8_t elevationLevel[16]; // level of each GBA elevation, quarter blocks
};

// A map connected to the current one. The game copies only MAP_OFFSET
// blocks of it into its grid; the camera sees further, so the field reads
// the rest from the connected layout (and its levels from its table).
struct Neighbor
{
    const struct MapLayout *layout;
    const struct FieldTable *table; // NULL: none (yet)
    int layoutId;
    int direction, offset;
    bool sameTilesets; // else its own secondary metatiles mean nothing here
};

// World sprites as they were when the OAM on screen was built (see Platform_OnLoadOam)
struct OamSnapshot
{
    bool valid;
    int32_t originX, originY;    // world pixel at the screen's top-left corner
    uint8_t spriteIds[PPU_OAM_COUNT];
    bool world[MAX_SPRITES];     // sprite follows the camera (coordOffsetEnabled)
    int16_t feetY[MAX_SPRITES];  // screen Y of the sprite's bottom edge, without y2
    int16_t centerX[MAX_SPRITES]; // screen X of the sprite's center, without x2
    s16 scroll[2];               // map BG scroll set for this frame (FieldUpdateBgTilemapScroll)
    uint8_t flat[MAX_SPRITES];   // FLAT_*: drawn lying on the ground
    uint8_t elevation[MAX_SPRITES]; // elevation of the person a sprite shows (0: not a person)
    uint8_t footprint[MAX_SPRITES]; // width of the object a sprite shows, for its contact shadow (0: none)
    bool object[MAX_SPRITES];    // shows a person, Pokémon or thing on the map (paper figure)
    float tilt[MAX_SPRITES];     // paper style: how far it leans in this step (radians)
};

enum { FLAT_NO, FLAT_GROUND, FLAT_REFLECTION };

static const uint8_t sObjSizes[3][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

static char sDataDir[512];
static struct FieldTable sTables[TABLE_CACHE];
static struct FieldTable *sTable;     // the current layout's (NULL: none)
static uint32_t sTableClock;
static struct Neighbor sNeighbors[MAX_NEIGHBORS];
static int sNeighborCount;
static int sNeighborsMap = -1;        // map (group << 8 | number) they were found for
static uint32_t sFrames;
static float sCameraLevel;
static int sCameraLayoutId = -1;
static bool sMeshGrass = true;        // the mesh has standing grass
static int16_t sObjectCells[OBJECT_EVENTS_COUNT][2];
static bool sObjectCellsValid[OBJECT_EVENTS_COUNT];
static uint32_t sParticleFrame;
static int sParticleLayoutId = -1;
static struct Field3DParticle sContacts[MAX_CONTACTS];
static int sContactCount;
static struct Field3DParticle sDrawParticles[FIELD3D_MAX_PARTICLES];

// Door animations draw their frames straight into the BG tilemap
// (DrawDoorMetatileAt); the 3D field shows them on the door's cells with
// texture rows 1024 and up
struct DoorOverride
{
    bool used;
    int16_t x, y;
    uint16_t entries[8];
};
static struct DoorOverride sDoors[FIELD3D_MAX_DOORS];
static struct DoorOverride sDoorsInMesh[FIELD3D_MAX_DOORS]; // the doors the mesh's texture rows show
static bool sDoorsChanged;
static uint16_t sMetatileEntries[FIELD3D_METATILE_ROWS * 8];
static uint32_t sMetatileVersion;
static const struct MapLayout *sMetatileLayout;
static uint16_t *sGrid;
static int sGridWidth, sGridHeight;
static struct Field3DVertex *sVertices;
static int sVertexCount, sVertexCapacity;
static int32_t *sCellFirst;  // first vertex of each grid cell's own images (its layers), -1: none
static int32_t *sCellEnd;
static uint32_t sMeshVersion, sPatchVersion;
static struct OamSnapshot sSnapshot;

static void TablePath(char *path, size_t size, int layoutId)
{
    snprintf(path, size, "%s/layout_%d.fld", sDataDir, layoutId);
}

int Field3D_Init(const char *dataDir)
{
    char path[700];
    char magic[4];
    FILE *file;
    bool ok;
    int i;

    for (i = 0; i < TABLE_CACHE; i++)
        sTables[i].layoutId = -1;
    snprintf(sDataDir, sizeof(sDataDir), "%s", dataDir);
    TablePath(path, sizeof(path), 1);
    file = fopen(path, "rb");
    if (file == NULL)
    {
        printf("field3d: no tables in %s (run hd2d/tools/mapconv/mapconv.py build); the field stays 2D\n", dataDir);
        return FIELD3D_DATA_MISSING;
    }
    ok = fread(magic, 1, 4, file) == 4 && memcmp(magic, "FLD5", 4) == 0;
    fclose(file);
    if (!ok)
    {
        printf("field3d: the tables in %s are from an older build; the field stays 2D\n", dataDir);
        return FIELD3D_DATA_OLD;
    }
    printf("field3d: tables from %s\n", dataDir);
    return FIELD3D_DATA_OK;
}

void Field3D_SetEnabled(bool enabled)
{
    HdSettings_Set(HD_OPT_FIELD3D, enabled);
}

bool Field3D_IsEnabled(void)
{
    return HdSettings_Get(HD_OPT_FIELD3D);
}

// CB2_OverworldBasic: the field while a battle transition plays
static bool InField(void)
{
    return (gMain.callback2 == CB2_Overworld || gMain.callback2 == CB2_OverworldBasic)
        && gMapHeader.mapLayout != NULL && gBackupMapLayout.map != NULL;
}

// GBA sprites that are really marks on the ground
static int FlatKind(const struct Sprite *sprite)
{
    static const u8 sGround[] = {
        FLDEFFOBJ_SHADOW_S, FLDEFFOBJ_SHADOW_M, FLDEFFOBJ_SHADOW_L, FLDEFFOBJ_SHADOW_XL, FLDEFFOBJ_RIPPLE,
        FLDEFFOBJ_SAND_FOOTPRINTS, FLDEFFOBJ_DEEP_SAND_FOOTPRINTS, FLDEFFOBJ_BIKE_TIRE_TRACKS,
    };
    size_t i;

    if (IsReflectionSprite(sprite))
        return FLAT_REFLECTION;
    for (i = 0; i < sizeof(sGround); i++)
    {
        if (sprite->template == gFieldEffectObjectTemplatePointers[sGround[i]])
            return FLAT_GROUND;
    }
    return FLAT_NO;
}

// Screen Y of a sprite's bottom edge, without y2 (hops). Sprites made of
// subsprites (the truck, the S.S. Tidal) are bigger than their own OAM entry:
// their pieces hang from the sprite's center (AddSubspritesToOamBuffer).
static int FeetY(const struct Sprite *sprite)
{
    const struct SubspriteTable *table = sprite->subspriteTables != NULL && sprite->subspriteMode != SUBSPRITES_OFF
                                       ? &sprite->subspriteTables[sprite->subspriteTableNum] : NULL;

    if (table != NULL && table->subsprites != NULL && table->subspriteCount > 0)
    {
        bool vflip = (sprite->oam.matrixNum >> 4) & 1;
        int i, bottom = -1000;

        for (i = 0; i < table->subspriteCount; i++)
        {
            const struct Subsprite *sub = &table->subsprites[i];
            int h = sObjSizes[sub->shape % 3][sub->size][1];
            int top = vflip ? -(sub->y + h) : sub->y;
            if (top + h > bottom)
                bottom = top + h;
        }
        return sprite->y + bottom + gSpriteCoordOffsetY;
    }
    return sprite->y + sprite->centerToCornerVecY + sObjSizes[sprite->oam.shape % 3][sprite->oam.size][1]
         + gSpriteCoordOffsetY;
}

// Paper figures rock from side to side as they walk: they lean while a step
// is under way (between two cells), one way on even cells, the other way on
// odd ones
static float StepTilt(const struct ObjectEvent *obj, int spriteId)
{
    int dx = abs(obj->currentCoords.x * 16 + 8 - (sSnapshot.centerX[spriteId] + sSnapshot.originX));
    int dy = abs(obj->currentCoords.y * 16 + 16 - (sSnapshot.feetY[spriteId] + sSnapshot.originY));
    int left = dx > dy ? dx : dy; // pixels to the cell it is stepping into

    if (left == 0 || left >= 16)
        return 0.0f;
    return sinf((1.0f - left / 16.0f) * (float)M_PI) * STEP_TILT
         * (((obj->currentCoords.x + obj->currentCoords.y) & 1) ? 1.0f : -1.0f);
}

void Platform_OnLoadOam(void)
{
    int i;

    sSnapshot.valid = InField();
    if (!sSnapshot.valid)
        return;
    FieldCamera_GetScreenOrigin(&sSnapshot.originX, &sSnapshot.originY);
    GetCameraOffsetWithPan(&sSnapshot.scroll[0], &sSnapshot.scroll[1]);
    memcpy(sSnapshot.spriteIds, gOamBufferSpriteIds, sizeof(sSnapshot.spriteIds));
    for (i = 0; i < MAX_SPRITES; i++)
    {
        const struct Sprite *sprite = &gSprites[i];
        sSnapshot.world[i] = sprite->inUse && sprite->coordOffsetEnabled;
        sSnapshot.flat[i] = FLAT_NO;
        sSnapshot.elevation[i] = 0;
        sSnapshot.footprint[i] = 0;
        sSnapshot.object[i] = false;
        sSnapshot.tilt[i] = 0.0f;
        if (sSnapshot.world[i])
        {
            sSnapshot.flat[i] = FlatKind(sprite);
            sSnapshot.feetY[i] = FeetY(sprite);
            sSnapshot.centerX[i] = sprite->x + gSpriteCoordOffsetX;
        }
    }
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        const struct ObjectEvent *obj = &gObjectEvents[i];
        const struct ObjectEventGraphicsInfo *info;
        if (!obj->active || obj->spriteId >= MAX_SPRITES)
            continue;
        sSnapshot.elevation[obj->spriteId] = obj->currentElevation;
        info = GetObjectEventGraphicsInfo(obj->graphicsId);
        if (obj->graphicsId == OBJ_EVENT_GFX_SUBMARINE_SHADOW)
        {
            sSnapshot.flat[obj->spriteId] = FLAT_GROUND; // a shadow in the water
            continue;
        }
        if (!sSnapshot.world[obj->spriteId])
            continue;
        sSnapshot.object[obj->spriteId] = true;
        if (!obj->invisible && info->width <= 32 && gSprites[obj->spriteId].y2 >= 0)
        {
            sSnapshot.footprint[obj->spriteId] = info->width; // on the ground: a contact shadow
            sSnapshot.tilt[obj->spriteId] = StepTilt(obj, obj->spriteId);
        }
    }
}

static uint16_t Read16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

// Changes whenever the file is rewritten (nanoseconds where the system has them)
static int64_t FileTime(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return 0;
#ifdef __linux__
    return (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec + st.st_size;
#else
    return (int64_t)st.st_mtime * 1000000 + st.st_size;
#endif
}

// Reads a layout's table into a cache slot; table->loaded tells if it worked
static void ReadTable(struct FieldTable *table, int layoutId)
{
    char path[700];
    FILE *file;
    uint8_t header[14];
    size_t cells;

    free(table->floor);
    free(table->level);
    free(table->base);
    table->floor = NULL;
    table->level = table->base = NULL;
    table->layoutId = layoutId;
    table->loaded = false;
    table->lastUse = ++sTableClock;

    TablePath(path, sizeof(path), layoutId);
    table->time = FileTime(path);
    file = fopen(path, "rb");
    if (file == NULL)
        return;
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "FLD5", 4) != 0)
    {
        fclose(file);
        fprintf(stderr, "field3d: %s is not an FLD5 file (run hd2d/tools/mapconv/mapconv.py build)\n", path);
        return;
    }
    table->width = Read16(header + 6);
    table->height = Read16(header + 8);
    table->defaultFloor = Read16(header + 10);
    table->particleTheme = Read16(header + 12);
    cells = (size_t)table->width * table->height;
    table->floor = malloc(cells * sizeof(uint16_t));
    table->level = malloc(cells);
    table->base = malloc(cells);
    if (fread(table->placement, 1, sizeof(table->placement), file) != sizeof(table->placement)
     || fread(table->cutMask, 1, sizeof(table->cutMask), file) != sizeof(table->cutMask)
     || fread(table->floor, sizeof(uint16_t), cells, file) != cells
     || fread(table->level, 1, cells, file) != cells
     || fread(table->base, 1, cells, file) != cells
     || fread(table->elevationLevel, 1, sizeof(table->elevationLevel), file) != sizeof(table->elevationLevel))
    {
        fclose(file);
        fprintf(stderr, "field3d: %s is truncated\n", path);
        return;
    }
    fclose(file);
    table->loaded = true;
    printf("field3d: layout %d (%dx%d) from %s\n", layoutId, table->width, table->height, path);
}

static bool InUse(const struct FieldTable *table)
{
    int i;

    if (table == sTable)
        return true;
    for (i = 0; i < sNeighborCount; i++)
    {
        if (sNeighbors[i].table == table)
            return true;
    }
    return false;
}

// The cache slot of a layout, read now if it is not there (or read again,
// when it changed: PC map editor). NULL if the layout has no table.
static struct FieldTable *GetTable(int layoutId, bool reload)
{
    struct FieldTable *slot = NULL;
    int i;

    for (i = 0; i < TABLE_CACHE; i++)
    {
        if (sTables[i].layoutId == layoutId)
        {
            slot = &sTables[i];
            break;
        }
    }
    if (slot == NULL)
    {
        // the least recently used one that nothing points to
        for (i = 0; i < TABLE_CACHE; i++)
        {
            if (!InUse(&sTables[i]) && (slot == NULL || sTables[i].lastUse < slot->lastUse))
                slot = &sTables[i];
        }
        if (slot == NULL)
            return NULL;
        ReadTable(slot, layoutId);
    }
    else if (reload)
    {
        ReadTable(slot, layoutId);
    }
    slot->lastUse = ++sTableClock;
    return slot->loaded ? slot : NULL;
}

// The maps connected to the current one, from its header
static void FindNeighbors(void)
{
    const struct MapConnections *connections = gMapHeader.connections;
    int i;

    sNeighborCount = 0;
    sNeighborsMap = (gSaveBlock1Ptr->location.mapGroup << 8) | gSaveBlock1Ptr->location.mapNum;
    if (connections == NULL)
        return;
    for (i = 0; i < connections->count && sNeighborCount < MAX_NEIGHBORS; i++)
    {
        const struct MapConnection *c = &connections->connections[i];
        const struct MapHeader *header;
        struct Neighbor *n;

        if (c->direction < CONNECTION_SOUTH || c->direction > CONNECTION_EAST)
            continue; // dive and emerge
        header = Overworld_GetMapHeaderByGroupAndId(c->mapGroup, c->mapNum);
        if (header == NULL || header->mapLayout == NULL || header->mapLayout->map == NULL)
            continue;
        n = &sNeighbors[sNeighborCount++];
        n->layout = header->mapLayout;
        n->layoutId = header->mapLayoutId;
        n->direction = c->direction;
        n->offset = (s32)c->offset;
        n->sameTilesets = header->mapLayout->primaryTileset == gMapHeader.mapLayout->primaryTileset
                       && header->mapLayout->secondaryTileset == gMapHeader.mapLayout->secondaryTileset;
        n->table = NULL;
    }
}

// The neighbors' tables, read along with the map's own (the map just left
// is usually still cached): one mesh build per new map
static void LoadNeighborTables(void)
{
    int i;

    for (i = 0; i < sNeighborCount; i++)
    {
        struct Neighbor *n = &sNeighbors[i];
        n->table = GetTable(n->layoutId, false);
        if (n->table != NULL && n->table->width != n->layout->width)
            n->table = NULL;
    }
}

// The neighbor at map coordinates (mx, my) outside the layout, and the cell
// of it there
static const struct Neighbor *NeighborAt(int mx, int my, int *cx, int *cy)
{
    int i;

    for (i = 0; i < sNeighborCount; i++)
    {
        const struct Neighbor *n = &sNeighbors[i];
        int w = n->layout->width, h = n->layout->height;

        switch (n->direction)
        {
        case CONNECTION_NORTH:
            if (my >= 0)
                continue;
            *cx = mx - n->offset;
            *cy = h + my;
            break;
        case CONNECTION_SOUTH:
            if (my < sTable->height)
                continue;
            *cx = mx - n->offset;
            *cy = my - sTable->height;
            break;
        case CONNECTION_WEST:
            if (mx >= 0)
                continue;
            *cx = w + mx;
            *cy = my - n->offset;
            break;
        default:
            if (mx < sTable->width)
                continue;
            *cx = mx - sTable->width;
            *cy = my - n->offset;
            break;
        }
        if (*cx >= 0 && *cy >= 0 && *cx < w && *cy < h)
            return n;
    }
    return NULL;
}

void Platform_OnDoorTiles(int x, int y, const u16 *tiles)
{
    int i, free = -1;

    for (i = 0; i < FIELD3D_MAX_DOORS; i++)
    {
        if (sDoors[i].used && sDoors[i].x == x && sDoors[i].y == y)
            break;
        if (!sDoors[i].used && free < 0)
            free = i;
    }
    if (i == FIELD3D_MAX_DOORS)
    {
        if (free < 0)
            return;
        i = free;
    }
    sDoors[i].used = true;
    sDoors[i].x = x;
    sDoors[i].y = y;
    memcpy(sDoors[i].entries, tiles, sizeof(sDoors[i].entries));
    sDoorsChanged = true;
}

void Platform_OnMapCellRedrawn(int x, int y)
{
    int i;

    for (i = 0; i < FIELD3D_MAX_DOORS; i++)
    {
        if (sDoors[i].used && ((sDoors[i].x == x && sDoors[i].y == y) || x < 0))
        {
            sDoors[i].used = false;
            sDoorsChanged = true;
        }
    }
}

// Texture row a cell's images come from: its metatile, or a door frame
static uint16_t TextureRow(int x, int y, uint16_t metatile)
{
    int i;

    for (i = 0; i < FIELD3D_MAX_DOORS; i++)
    {
        if (sDoors[i].used && sDoors[i].x == x && sDoors[i].y == y)
            return 1024 + i;
    }
    return metatile;
}

static void CopyDoorEntries(void)
{
    int i;

    for (i = 0; i < FIELD3D_MAX_DOORS; i++)
        memcpy(&sMetatileEntries[(1024 + i) * 8], sDoors[i].entries, sizeof(sDoors[i].entries));
    sMetatileVersion++;
}

static void CopyMetatileEntries(const struct MapLayout *layout)
{
    int i;

    memset(sMetatileEntries, 0, sizeof(sMetatileEntries));
    for (i = 0; i < 1024; i++)
    {
        const struct Tileset *tileset = i < NUM_METATILES_IN_PRIMARY ? layout->primaryTileset : layout->secondaryTileset;
        int local = i % NUM_METATILES_IN_PRIMARY;
        // Only metatiles the table knows about: the tileset arrays may be shorter than 512
        if (tileset == NULL || tileset->metatiles == NULL)
            continue;
        if ((sTable->placement[i][0] >> 4) == KIND_NONE && (sTable->placement[i][1] >> 4) == KIND_NONE)
            continue;
        memcpy(&sMetatileEntries[i * 8], &tileset->metatiles[local * 8], 8 * sizeof(uint16_t));
    }
    sMetatileLayout = layout;
    CopyDoorEntries();
}
static struct Field3DVertex *AddQuad(void)
{
    if (sVertexCount + 6 > sVertexCapacity)
    {
        sVertexCapacity = sVertexCapacity ? sVertexCapacity * 2 : 6 * 4096;
        sVertices = realloc(sVertices, sVertexCapacity * sizeof(*sVertices));
    }
    sVertexCount += 6;
    return &sVertices[sVertexCount - 6];
}

// corners: 4 positions TL, TR, BL, BR of the 16x16 image (or of uv, when
// given). hflip mirrors the image; the top corners get swayTop.
static void EmitQuadUv(const float corners[4][3], const float uv[4][2], uint16_t metatile, uint16_t floor, uint8_t layer,
                       uint8_t flags, uint16_t cutMask, bool hflip, uint8_t swayTop)
{
    static const int sOrder[6] = {0, 1, 2, 1, 3, 2};
    static const float sUv[4][2] = {{0, 0}, {16, 0}, {0, 16}, {16, 16}};
    struct Field3DVertex *v = AddQuad();
    int i;

    if (uv == NULL)
        uv = sUv;
    for (i = 0; i < 6; i++)
    {
        int c = sOrder[i];
        memcpy(v[i].pos, corners[c], sizeof(v[i].pos));
        v[i].uv[0] = hflip ? 16 - uv[c][0] : uv[c][0];
        v[i].uv[1] = uv[c][1];
        v[i].metatile = metatile;
        v[i].floor = floor;
        v[i].layer = layer;
        v[i].flags = flags;
        v[i].sway = c < 2 ? swayTop : 0;
        v[i].unused = 0;
        v[i].cutMask = cutMask;
    }
}

static void EmitQuad(const float corners[4][3], uint16_t metatile, uint16_t floor, uint8_t layer, uint8_t flags,
                     uint16_t cutMask, bool hflip, uint8_t swayTop)
{
    EmitQuadUv(corners, NULL, metatile, floor, layer, flags, cutMask, hflip, swayTop);
}

// Flat at height y; it casts a shadow only when raised above the ground
// under it (roofs, the tops of rock masses)
static void EmitFlatOver(float x, float z, float y, float ground, uint16_t metatile, uint16_t floor, uint8_t layer)
{
    const float c[4][3] = {{x, y, z}, {x + 1, y, z}, {x, y, z + 1}, {x + 1, y, z + 1}};
    EmitQuad(c, metatile, floor, layer, y > ground + 0.05f ? 0 : FIELD3D_FLAG_NOSHADOW, 0, false, 0);
}

static void EmitFlat(float x, float z, float y, uint16_t metatile, uint16_t floor, uint8_t layer)
{
    EmitFlatOver(x, z, y, y, metatile, floor, layer);
}

// Upright, facing south, standing on the line Z = z
static void EmitUpright(float x, float z, float y0, uint16_t metatile, uint16_t floor, uint8_t layer, uint8_t flags,
                        uint16_t cutMask)
{
    const float c[4][3] = {{x, y0 + 1, z}, {x + 1, y0 + 1, z}, {x, y0, z}, {x + 1, y0, z}};
    EmitQuad(c, metatile, floor, layer, flags | FIELD3D_FLAG_UPRIGHT, cutMask, false, 0);
}

// Standing grass: the metatile, minus its cut colors, as two rows of blades
// that sway; the floor goes under it
static void EmitGrass(float x, float z, float y, uint16_t metatile, uint16_t floor, uint16_t cutMask)
{
    int row;

    EmitFlat(x, z, y, floor, floor, 0);
    for (row = 0; row < 2; row++)
    {
        float front = z + 0.5f + row * 0.5f;
        const float c[4][3] = {{x, y + GRASS_HEIGHT, front}, {x + 1, y + GRASS_HEIGHT, front},
                               {x, y - 0.05f, front}, {x + 1, y - 0.05f, front}};
        EmitQuad(c, metatile, floor, 0, FIELD3D_FLAG_UPRIGHT, cutMask, row == 0, 255);
    }
}


// The block the game shows at (x, y). Past the part of the connected maps the
// game copied into its grid, the rest of them (with the same tilesets, or
// their primary metatiles); elsewhere the layout's 2x2 border pattern
// (GetBorderBlockAt in fieldmap.c).
static uint16_t GridBlock(int x, int y)
{
    const struct Neighbor *n;
    int cx, cy;

    if (x >= 0 && y >= 0 && x < sGridWidth && y < sGridHeight)
    {
        uint16_t block = sGrid[x + y * sGridWidth];
        if (block != MAPGRID_UNDEFINED)
            return block;
    }
    else if ((n = NeighborAt(x - MAP_OFFSET, y - MAP_OFFSET, &cx, &cy)) != NULL)
    {
        uint16_t block = n->layout->map[cx + cy * n->layout->width];
        if (n->sameTilesets || (block & MAPGRID_METATILE_ID_MASK) < NUM_METATILES_IN_PRIMARY)
            return block;
        if (n->table != NULL && n->table->floor[cx + cy * n->table->width] < NUM_METATILES_IN_PRIMARY)
            return n->table->floor[cx + cy * n->table->width];
    }
    if (gMapHeader.mapLayout->border == NULL)
        return MAPGRID_UNDEFINED;
    return gMapHeader.mapLayout->border[((x + 1) & 1) + (((y + 1) & 1) << 1)] | MAPGRID_IMPASSABLE;
}

static bool IsFlat(int kind)
{
    return kind == KIND_GROUND || kind == KIND_TOP || kind == KIND_WATER || kind == KIND_GRASS;
}

static int BottomKind(int x, int y)
{
    return sTable->placement[GridBlock(x, y) & MAPGRID_METATILE_ID_MASK][0] >> 4;
}

// What the side of a raised cell is made of: the first wall below it in its
// column (rock under a cliff top, supports under a bridge), else its own ground
static uint16_t SideTexture(int x, int y, uint16_t fallback)
{
    int k;

    for (k = 1; k <= 6; k++)
    {
        int kind = BottomKind(x, y + k);
        if (kind == KIND_FACE)
            return GridBlock(x, y + k) & MAPGRID_METATILE_ID_MASK;
        if (kind != KIND_TOP)
            break;
    }
    return fallback;
}

// Level (blocks) of a cell outside the layout (map coordinates): a connected
// map's, from its table; elsewhere (border) the ground goes on at the level
// of the nearest edge cell's ground, so edges do not drop off
static float OutsideLevel(int mx, int my, bool base)
{
    int cx, cy;
    const struct Neighbor *n = NeighborAt(mx, my, &cx, &cy);

    if (n != NULL && n->table != NULL)
        return (base ? n->table->base : n->table->level)[cx + cy * n->table->width] * 0.25f;
    cx = mx < 0 ? 0 : (mx >= sTable->width ? sTable->width - 1 : mx);
    cy = my < 0 ? 0 : (my >= sTable->height ? sTable->height - 1 : my);
    return sTable->base[cx + cy * sTable->width] * 0.25f;
}

// Level (blocks) of a grid cell: where it lies (base = false) or the ground
// under it (base = true)
static float CellLevel(bool base, int x, int y)
{
    int mx = x - MAP_OFFSET, my = y - MAP_OFFSET;

    if (mx < 0 || my < 0 || mx >= sTable->width || my >= sTable->height)
        return OutsideLevel(mx, my, base);
    return (base ? sTable->base : sTable->level)[mx + my * sTable->width] * 0.25f;
}

// The floor under a cell (what is drawn beneath things standing on it)
static uint16_t CellFloor(int x, int y)
{
    int mx = x - MAP_OFFSET, my = y - MAP_OFFSET, cx, cy;
    const struct Neighbor *n;

    if (mx >= 0 && my >= 0 && mx < sTable->width && my < sTable->height)
        return sTable->floor[mx + my * sTable->width];
    n = NeighborAt(mx, my, &cx, &cy);
    if (n != NULL && n->table != NULL)
    {
        uint16_t floor = n->table->floor[cx + cy * n->table->width];
        if (n->sameTilesets || floor < NUM_METATILES_IN_PRIMARY)
            return floor;
    }
    return sTable->defaultFloor;
}

// Height of the ground a cell shows: its level if it lies flat, else the
// ground under it
static float SurfaceAt(int x, int y)
{
    int kind = BottomKind(x, y);
    if (kind == KIND_WATER)
        return CellLevel(false, x, y) - WATER_DEPTH;
    return CellLevel(!IsFlat(kind), x, y);
}

// Where neighboring cells sit at different levels, the camera can look into
// the gap between them (from the side, near the screen edges, or in front
// of a raised area). Skirts close it, with the higher cell's ground repeated.
static void EmitSkirts(int x, int y, uint16_t texture, float surface)
{
    int side;

    for (side = 0; side < 2; side++)
    {
        int nx = side ? x + 1 : x - 1;
        float low = SurfaceAt(nx, y), edge = side ? x + 1 : x;
        if (surface > low + 0.01f)
        {
            float h = (surface - low) * 16;
            const float c[4][3] = {{edge, surface, y + surface}, {edge, surface, y + 1 + surface},
                                   {edge, low, y + low}, {edge, low, y + 1 + low}};
            const float uv[4][2] = {{0, 0}, {16, 0}, {0, h}, {16, h}};
            EmitQuadUv(c, uv, texture, texture, 0, FIELD3D_FLAG_UPRIGHT | FIELD3D_FLAG_REPEAT, 0, false, 0);
        }
    }
    // in front: down to the ground there, or to the top edge of a wall there
    // (it stands one block over its level)
    if (IsFlat(BottomKind(x, y + 1)) || BottomKind(x, y + 1) == KIND_FACE)
    {
        float low = IsFlat(BottomKind(x, y + 1)) ? SurfaceAt(x, y + 1) : CellLevel(false, x, y + 1) + 1.0f;
        if (surface > low + 0.01f)
        {
            float h = (surface - low) * 16;
            const float c[4][3] = {{x, surface, y + 1 + surface}, {x + 1, surface, y + 1 + surface},
                                   {x, low, y + 1 + low}, {x + 1, low, y + 1 + low}};
            const float uv[4][2] = {{0, 0}, {16, 0}, {0, h}, {16, h}};
            EmitQuadUv(c, uv, texture, texture, 0, FIELD3D_FLAG_UPRIGHT | FIELD3D_FLAG_REPEAT, 0, false, 0);
        }
    }
}

static void BuildMesh(void)
{
    int x, y, layer;
    size_t cells = (size_t)sGridWidth * sGridHeight;

    sVertexCount = 0;
    free(sCellFirst);
    free(sCellEnd);
    sCellFirst = malloc(cells * sizeof(*sCellFirst));
    sCellEnd = malloc(cells * sizeof(*sCellEnd));
    for (y = -VIEW_MARGIN; y < sGridHeight + VIEW_MARGIN; y++)
    {
        for (x = -VIEW_MARGIN; x < sGridWidth + VIEW_MARGIN; x++)
        {
            uint16_t block = GridBlock(x, y);
            uint16_t metatile = block & MAPGRID_METATILE_ID_MASK;
            bool inGrid = x >= 0 && y >= 0 && x < sGridWidth && y < sGridHeight;
            uint16_t floor = CellFloor(x, y);
            // A cell at level L is drawn L blocks up and L blocks south: from
            // the camera's angle it stays about where the GBA draws it
            float level = CellLevel(false, x, y);
            float base = CellLevel(true, x, y);
            float z = y + level;
            int bottomKind = sTable->placement[metatile][0] >> 4;
            uint16_t texture = TextureRow(x, y, metatile); // placement from the metatile, images maybe from a door

            // things standing up have the ground of their base under them
            if (!IsFlat(bottomKind))
                EmitFlat(x, y + base, base, floor, floor, 0);
            EmitSkirts(x, y, SideTexture(x, y, IsFlat(bottomKind) && bottomKind != KIND_WATER ? metatile : floor),
                       SurfaceAt(x, y));

            // Both layers on one plane: the top one is drawn after the bottom one
            // at the same depth and wins (glDepthFunc(GL_LEQUAL)). Lifting it
            // instead opened slits where it met a neighbor's bottom layer.
            if (inGrid)
                sCellFirst[x + y * sGridWidth] = sVertexCount;
            for (layer = 0; layer < 2; layer++)
            {
                int kind = sTable->placement[metatile][layer] >> 4;
                int rise = sTable->placement[metatile][layer] & 15;

                if (kind == KIND_FACE)
                    kind = KIND_CARD;
                else if (kind == KIND_TOP)
                    kind = KIND_GROUND;

                switch (kind)
                {
                case KIND_GROUND:
                    EmitFlatOver(x, z, level, base + RAISED, texture, floor, layer);
                    break;
                case KIND_WATER:
                    EmitFlat(x, z, level - WATER_DEPTH, texture, floor, layer);
                    break;
                case KIND_CARD:
                case KIND_WALL:
                    EmitUpright(x, z + rise + 1, level + rise, texture, floor, layer,
                                (kind == KIND_CARD && layer == 0) ? FIELD3D_FLAG_CUTOUT : 0,
                                kind == KIND_CARD ? sTable->cutMask[metatile][layer] : 0);
                    break;
                case KIND_ROOF:
                    EmitFlatOver(x, z + rise, level + rise, base, texture, floor, layer);
                    break;
                case KIND_GRASS:
                    if (sMeshGrass)
                        EmitGrass(x, z, level, texture, floor, sTable->cutMask[metatile][layer]);
                    else
                        EmitFlat(x, z, level, texture, floor, layer);
                    break;
                }
            }
            if (inGrid)
                sCellEnd[x + y * sGridWidth] = sVertexCount;
        }
    }
    memcpy(sDoorsInMesh, sDoors, sizeof(sDoorsInMesh));
    sMeshVersion++;
}

// Points a cell's own images at its texture row now (door frame or metatile)
static void RetextureCell(int x, int y, int *first, int *end)
{
    uint16_t metatile, texture;
    int i;

    if (x < 0 || y < 0 || x >= sGridWidth || y >= sGridHeight)
        return;
    metatile = GridBlock(x, y) & MAPGRID_METATILE_ID_MASK;
    texture = TextureRow(x, y, metatile);
    for (i = sCellFirst[x + y * sGridWidth]; i < sCellEnd[x + y * sGridWidth]; i++)
    {
        if (sVertices[i].metatile == metatile || sVertices[i].metatile >= 1024)
            sVertices[i].metatile = texture;
    }
    if (sCellFirst[x + y * sGridWidth] < *first)
        *first = sCellFirst[x + y * sGridWidth];
    if (sCellEnd[x + y * sGridWidth] > *end)
        *end = sCellEnd[x + y * sGridWidth];
}

// A door frame changed: only the door cells change texture rows, no rebuild
// (on the Switch a rebuild takes a frame or more)
static void PatchDoors(struct Field3DFrame *frame)
{
    int i, first = sVertexCount, end = 0;

    for (i = 0; i < FIELD3D_MAX_DOORS; i++)
    {
        if (sDoorsInMesh[i].used)
            RetextureCell(sDoorsInMesh[i].x, sDoorsInMesh[i].y, &first, &end);
        if (sDoors[i].used)
            RetextureCell(sDoors[i].x, sDoors[i].y, &first, &end);
    }
    memcpy(sDoorsInMesh, sDoors, sizeof(sDoorsInMesh));
    if (end > first)
    {
        frame->patchFirst = first;
        frame->patchCount = end - first;
        sPatchVersion++;
    }
}

// Copies the map grid; returns true if it changed (puzzles, new map)
static bool SyncGrid(void)
{
    size_t cells = (size_t)gBackupMapLayout.width * gBackupMapLayout.height;

    if (gBackupMapLayout.width != sGridWidth || gBackupMapLayout.height != sGridHeight)
    {
        free(sGrid);
        sGrid = malloc(cells * sizeof(uint16_t));
        sGridWidth = gBackupMapLayout.width;
        sGridHeight = gBackupMapLayout.height;
        memcpy(sGrid, gBackupMapLayout.map, cells * sizeof(uint16_t));
        return true;
    }
    if (memcmp(sGrid, gBackupMapLayout.map, cells * sizeof(uint16_t)) == 0)
        return false;
    memcpy(sGrid, gBackupMapLayout.map, cells * sizeof(uint16_t));
    return true;
}

// Level (blocks) of the cell under a world point (gBackupMapLayout blocks)
static float LevelAt(float wx, float wz)
{
    return CellLevel(false, (int)floorf(wx), (int)floorf(wz));
}

// Level (blocks) a person stands at: the cell's, except on two-level bridge
// cells (elevation 15), where it is their own elevation's: on the deck or
// under it. *roof gets the level above their head there (the deck), else -1.
static float PersonLevel(float wx, float wz, int elevation, float *roof)
{
    int gx = (int)floorf(wx), gy = (int)floorf(wz);
    float cell = LevelAt(wx, wz);

    *roof = -1.0f;
    if (elevation >= 1 && elevation <= 14 && MapGridGetElevationAt(gx, gy) == 15)
    {
        float own = sTable->elevationLevel[elevation] * 0.25f;
        if (own < cell)
            *roof = cell;
        return own;
    }
    return cell;
}

// A soft dark ellipse on the ground under someone's feet: the sun's shadow
// falls to one side, and without this people seem to float over the ground
static void AddContactShadow(int spriteId, float level)
{
    struct Field3DParticle *p;
    float x = (sSnapshot.centerX[spriteId] + sSnapshot.originX) / 16.0f;
    float z = (sSnapshot.feetY[spriteId] + sSnapshot.originY) / 16.0f - 0.5f;
    int width = sSnapshot.footprint[spriteId] < 24 ? sSnapshot.footprint[spriteId] : 24;

    if (sContactCount == MAX_CONTACTS || BottomKind((int)floorf(x), (int)floorf(z)) == KIND_WATER)
        return;
    p = &sContacts[sContactCount++];
    p->pos[0] = x;
    p->pos[1] = level + 0.02f;
    p->pos[2] = z + level + 0.06f;
    p->size = width / 16.0f * 0.38f;
    p->angle = 0;
    p->color[0] = 18;
    p->color[1] = 22;
    p->color[2] = 40;
    p->color[3] = (uint8_t)(CONTACT_ALPHA * 255);
    p->shape = FIELD3D_PARTICLE_CONTACT;
    p->flags = FIELD3D_PARTICLE_FLAT;
}

// Where an OAM entry sits on screen (as the PPU places it)
static void OamRect(const struct OamData *oam, int *x, int *y, int *w, int *h)
{
    *w = sObjSizes[oam->shape % 3][oam->size][0];
    *h = sObjSizes[oam->shape % 3][oam->size][1];
    *x = oam->x;
    *y = oam->y;
    if (*x >= DISPLAY_WIDTH)
        *x -= 512;
    if (*y >= DISPLAY_HEIGHT)
        *y -= 256;
    if (oam->affineMode == ST_OAM_AFFINE_DOUBLE)
    {
        // drawn untransformed for now, centered in its double-size box
        *x += *w / 2;
        *y += *h / 2;
    }
}

static bool Overlap(int a0, int a1, int b0, int b1)
{
    return a0 < b1 && b0 < a1;
}

// The sides of OAM entry i where no other piece of the same sprite goes on
// (the paper rim grows past those; past the others it would cut across the
// figure). Subsprites sit next to each other in the OAM.
static uint8_t OuterEdges(int i)
{
    const struct OamData *oams = (const struct OamData *)OAM;
    uint8_t edges = FIELD3D_EDGE_LEFT | FIELD3D_EDGE_RIGHT | FIELD3D_EDGE_TOP;
    int x, y, w, h, j;

    OamRect(&oams[i], &x, &y, &w, &h);
    for (j = i - 4; j <= i + 4; j++)
    {
        int ox, oy, ow, oh;
        if (j < 0 || j >= PPU_OAM_COUNT || j == i || sSnapshot.spriteIds[j] != sSnapshot.spriteIds[i])
            continue;
        OamRect(&oams[j], &ox, &oy, &ow, &oh);
        if (ox + ow == x && Overlap(y, y + h, oy, oy + oh))
            edges &= ~FIELD3D_EDGE_LEFT;
        if (ox == x + w && Overlap(y, y + h, oy, oy + oh))
            edges &= ~FIELD3D_EDGE_RIGHT;
        if (oy + oh == y && Overlap(x, x + w, ox, ox + ow))
            edges &= ~FIELD3D_EDGE_TOP;
    }
    return edges;
}

// Paper style: the little stand a figure is glued to, lying on the ground
// under its feet (over the contact shadow, which then shows around it)
static void AddPaperBase(struct Field3DFrame *frame, int spriteId, float level)
{
    struct Field3DBillboard *bb;
    float x = (sSnapshot.centerX[spriteId] + sSnapshot.originX) / 16.0f;
    float z = (sSnapshot.feetY[spriteId] + sSnapshot.originY) / 16.0f - 0.5f;
    int width = (sSnapshot.footprint[spriteId] < 24 ? sSnapshot.footprint[spriteId] : 24) * 3 / 4 + 2;
    int depth = width * 3 / 5;

    if (frame->billboardCount == FIELD3D_MAX_BILLBOARDS || BottomKind((int)floorf(x), (int)floorf(z)) == KIND_WATER)
        return;
    bb = &frame->billboards[frame->billboardCount++];
    memset(bb, 0, sizeof(*bb));
    bb->x0 = x - width / 32.0f;
    bb->x1 = x + width / 32.0f;
    bb->y0 = level + 0.024f;
    bb->z = z + level + 0.05f - depth / 32.0f;
    bb->y1 = z + level + 0.05f + depth / 32.0f;
    bb->width = width;
    bb->height = depth;
    bb->flags = FIELD3D_BB_FLAT | FIELD3D_BB_BASE;
    bb->oamIndex = 0xFF;
}

static void CollectBillboards(struct Field3DFrame *frame, uint8_t *skipOam, bool contacts, bool paper)
{
    const struct OamData *oams = (const struct OamData *)OAM;
    bool shadowed[MAX_SPRITES] = {false};
    bool based[MAX_SPRITES] = {false};
    int i;

    frame->billboardCount = 0;
    sContactCount = 0;
    if (!sSnapshot.valid)
        return;
    for (i = 0; i < PPU_OAM_COUNT && frame->billboardCount < FIELD3D_MAX_BILLBOARDS; i++)
    {
        const struct OamData *oam = &oams[i];
        uint8_t spriteId = sSnapshot.spriteIds[i];
        struct Field3DBillboard *bb;
        int x, y, w, h, feet;
        float level, roof;

        if (spriteId >= MAX_SPRITES || !sSnapshot.world[spriteId])
            continue;
        if (oam->affineMode == ST_OAM_AFFINE_ERASE || oam->shape == 3 || oam->objMode == ST_OAM_OBJ_WINDOW)
            continue;
        OamRect(oam, &x, &y, &w, &h);
        feet = sSnapshot.feetY[spriteId];

        if (sSnapshot.flat[spriteId] != FLAT_NO)
        {
            // a mark on the ground: the rectangle it covers on screen, laid on the
            // ground there (or on the water, for reflections)
            float cx = (x + w * 0.5f + sSnapshot.originX) / 16.0f;
            float z0 = (y + sSnapshot.originY) / 16.0f, z1 = (y + h + sSnapshot.originY) / 16.0f;
            float mid = (z0 + z1) * 0.5f;

            level = LevelAt(cx, mid);
            bb = &frame->billboards[frame->billboardCount++];
            memset(bb, 0, sizeof(*bb));
            bb->x0 = (x + sSnapshot.originX) / 16.0f;
            bb->x1 = bb->x0 + w / 16.0f;
            bb->y0 = SurfaceAt((int)floorf(cx), (int)floorf(mid)) + 0.012f + (PPU_OAM_COUNT - i) * 0.0002f;
            bb->z = z0 + level;
            bb->y1 = z1 + level;
            bb->tileNum = oam->tileNum;
            bb->width = w;
            bb->height = h;
            bb->cutTop = 0;
            bb->palette = oam->paletteNum;
            bb->flags = FIELD3D_BB_FLAT;
            if (oam->affineMode == ST_OAM_AFFINE_OFF)
            {
                if (oam->matrixNum & ST_OAM_HFLIP)
                    bb->flags |= FIELD3D_BB_HFLIP;
                if (oam->matrixNum & ST_OAM_VFLIP)
                    bb->flags |= FIELD3D_BB_VFLIP;
            }
            else if (sSnapshot.flat[spriteId] == FLAT_REFLECTION)
            {
                // the reflection matrices: 0 flips vertically, 1 both ways
                bb->flags |= FIELD3D_BB_VFLIP | ((oam->matrixNum & 31) == 1 ? FIELD3D_BB_HFLIP : 0);
            }
            if (oam->bpp)
                bb->flags |= FIELD3D_BB_8BPP;
            bb->oamIndex = i;
            skipOam[i] = 1;
            continue;
        }

        bb = &frame->billboards[frame->billboardCount++];
        memset(bb, 0, sizeof(*bb));
        bb->x0 = (x + sSnapshot.originX) / 16.0f;
        bb->x1 = bb->x0 + w / 16.0f;
        bb->z = (feet + sSnapshot.originY) / 16.0f - 0.5f;
        // stands on the level of the cell under its feet, moved like the cell
        level = PersonLevel((bb->x0 + bb->x1) * 0.5f, bb->z, sSnapshot.elevation[spriteId], &roof);
        bb->y0 = (feet - (y + h)) / 16.0f + level;
        bb->y1 = (feet - y) / 16.0f + level;
        bb->cutTop = 0;
        if (roof >= 0 && bb->y1 > roof)
        {
            // under a bridge: what would stick up through the deck is hidden
            bb->cutTop = (uint8_t)fminf(h, (bb->y1 - roof) * 16.0f + 0.5f);
            bb->y1 = roof;
        }
        bb->z += level + (PPU_OAM_COUNT - i) * SPRITE_BIAS;
        bb->tileNum = oam->tileNum;
        bb->width = w;
        bb->height = h;
        bb->palette = oam->paletteNum;
        bb->flags = 0;
        if (oam->affineMode == ST_OAM_AFFINE_OFF)
        {
            if (oam->matrixNum & ST_OAM_HFLIP)
                bb->flags |= FIELD3D_BB_HFLIP;
            if (oam->matrixNum & ST_OAM_VFLIP)
                bb->flags |= FIELD3D_BB_VFLIP;
        }
        if (oam->bpp)
            bb->flags |= FIELD3D_BB_8BPP;
        bb->oamIndex = i;
        skipOam[i] = 1;
        if (paper && sSnapshot.object[spriteId])
        {
            bb->flags |= FIELD3D_BB_PAPER;
            bb->edges = bb->cutTop > 0 ? OuterEdges(i) & ~FIELD3D_EDGE_TOP : OuterEdges(i);
            bb->tilt = sSnapshot.tilt[spriteId];
            bb->pivotX = (sSnapshot.centerX[spriteId] + sSnapshot.originX) / 16.0f;
            bb->pivotY = level;
            if (sSnapshot.footprint[spriteId] != 0 && !based[spriteId])
            {
                based[spriteId] = true;
                AddPaperBase(frame, spriteId, level);
            }
        }

        if (contacts && sSnapshot.footprint[spriteId] != 0 && !shadowed[spriteId])
        {
            shadowed[spriteId] = true;
            AddContactShadow(spriteId, level);
        }
        // People (16 px wide or more) part the grass
        if (w >= 16 && h >= 16 && frame->actorCount < FIELD3D_MAX_ACTORS)
        {
            frame->actors[frame->actorCount][0] = (bb->x0 + bb->x1) * 0.5f;
            frame->actors[frame->actorCount][1] = bb->z;
            frame->actorCount++;
        }
    }
}

static int LightingForMap(void)
{
    switch (gMapHeader.mapType)
    {
    case MAP_TYPE_INDOOR:
    case MAP_TYPE_SECRET_BASE:
        return FIELD3D_LIGHT_INDOOR;
    case MAP_TYPE_UNDERGROUND:
        return FIELD3D_LIGHT_CAVE;
    case MAP_TYPE_UNDERWATER:
        return FIELD3D_LIGHT_UNDERWATER;
    default:
        return FIELD3D_LIGHT_OUTDOOR;
    }
}

// How far the game has faded the screen to black or white through the
// palettes (warps, battles, menus). The map and the sprites get it from the
// palettes; particles, contact shadows and haze have colors of their own.
static void PaletteFade(float fade[2])
{
    const uint16_t *now = (const uint16_t *)PLTT;
    uint32_t sumNow = 0, sumBase = 0, full = 0;
    int i;

    for (i = 0; i < 512; i++)
    {
        uint16_t a = now[i], b = gPlttBufferUnfaded[i];
        sumNow += (a & 31) + ((a >> 5) & 31) + ((a >> 10) & 31);
        sumBase += (b & 31) + ((b >> 5) & 31) + ((b >> 10) & 31);
        full += 31 * 3;
    }
    fade[0] = 1.0f;
    fade[1] = 0.0f;
    if (sumNow < sumBase)
        fade[0] = (float)sumNow / sumBase;
    else if (sumNow > sumBase && full > sumBase)
        fade[1] = (float)(sumNow - sumBase) / (full - sumBase);
}

// The game's time of day in hours (0-24): the system clock, as the port's
// RTC reads it, minus the offset the player set on the clock at home
// (RtcCalcLocalTime does the same, but logs every call).
// PE_TEST_HOUR fixes it for tests.
static float GameHour(void)
{
    static int sTestMinutes = -2;
    time_t now;
    struct tm *t;
    int minutes;

    if (sTestMinutes == -2)
    {
        const char *value = getenv("PE_TEST_HOUR");
        sTestMinutes = value != NULL ? (int)(atof(value) * 60.0) : -1;
    }
    if (sTestMinutes >= 0)
        return (sTestMinutes % (24 * 60)) / 60.0f;
    now = time(NULL);
    t = localtime(&now);
    if (t == NULL)
        return 12.0f;
    minutes = t->tm_hour * 60 + t->tm_min
            - gSaveBlock2Ptr->localTimeOffset.hours * 60 - gSaveBlock2Ptr->localTimeOffset.minutes;
    minutes = ((minutes % (24 * 60)) + 24 * 60) % (24 * 60);
    return minutes / 60.0f + t->tm_sec / 3600.0f;
}

static bool IsNight(float hour)
{
    return hour >= 0 && (hour < 6.0f || hour >= 19.0f);
}

// Particles, blur, glow and haze belong to the open air: towns, routes and
// the sea; not to houses, buildings and caves
static bool OpenAir(void)
{
    switch (gMapHeader.mapType)
    {
    case MAP_TYPE_TOWN:
    case MAP_TYPE_CITY:
    case MAP_TYPE_ROUTE:
    case MAP_TYPE_OCEAN_ROUTE:
    case MAP_TYPE_UNDERWATER:
        return true;
    default:
        return false;
    }
}

// Overcast weather softens the sun (the game already darkens the palettes)
static float DimForWeather(void)
{
    switch (GetCurrentWeather())
    {
    case WEATHER_RAIN:
    case WEATHER_RAIN_THUNDERSTORM:
    case WEATHER_DOWNPOUR:
    case WEATHER_FOG_HORIZONTAL:
    case WEATHER_FOG_DIAGONAL:
    case WEATHER_SHADE:
    case WEATHER_ABNORMAL:
        return 1.0f;
    case WEATHER_VOLCANIC_ASH:
    case WEATHER_SANDSTORM:
        return 0.5f;
    default:
        return 0.0f;
    }
}

// The map editor rewrites tables; pick a new one up without a restart
// (table->time is the file time seen when it was read)
static bool TableChanged(int layoutId)
{
#ifdef __SWITCH__
    return false;
#else
    char path[700];
    int i;

    if (sFrames % RELOAD_FRAMES != 0)
        return false;
    for (i = 0; i < TABLE_CACHE; i++)
    {
        if (sTables[i].layoutId == layoutId)
        {
            TablePath(path, sizeof(path), layoutId);
            return FileTime(path) != sTables[i].time;
        }
    }
    return false;
#endif
}

// --- particles --------------------------------------------------------------

static uint32_t ParticleCellFlags(int x, int y)
{
    uint16_t block = GridBlock(x, y);
    int kind = sTable->placement[block & MAPGRID_METATILE_ID_MASK][0] >> 4;
    uint32_t behavior = MapGridGetMetatileBehaviorAt(x, y);
    uint32_t flags = 0;

    if (kind == KIND_WATER)
        flags |= PARTICLE_CELL_WATER;
    if (block & MAPGRID_COLLISION_MASK)
        flags |= PARTICLE_CELL_BLOCKED;
    if (MetatileBehavior_IsHotSprings(behavior))
        flags |= PARTICLE_CELL_HOTSPRING;
    // mist where a waterfall lands
    if (MetatileBehavior_IsWaterfall(behavior) && !MetatileBehavior_IsWaterfall(MapGridGetMetatileBehaviorAt(x, y + 1)))
        flags |= PARTICLE_CELL_WATERFALL;
    if (MetatileBehavior_IsTallGrass(behavior) || MetatileBehavior_IsLongGrass(behavior))
        flags |= PARTICLE_CELL_GRASS;
    if (MetatileBehavior_IsAshGrass(behavior))
        flags |= PARTICLE_CELL_ASHGRASS;
    if (MetatileBehavior_IsSandOrDeepSand(behavior))
        flags |= PARTICLE_CELL_SAND;
    return flags;
}

static float ParticleSurface(int x, int y)
{
    return SurfaceAt(x, y);
}

// Cells people (and Pokémon following the script) stepped into
static void CollectSteps(struct ParticleWorld *world)
{
    int i;

    world->stepCount = 0;
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        const struct ObjectEvent *obj = &gObjectEvents[i];
        if (!obj->active || obj->invisible)
        {
            sObjectCellsValid[i] = false;
            continue;
        }
        if (sObjectCellsValid[i] && (obj->currentCoords.x != sObjectCells[i][0] || obj->currentCoords.y != sObjectCells[i][1])
         && world->stepCount < PARTICLE_MAX_STEPS)
        {
            world->steps[world->stepCount][0] = obj->currentCoords.x;
            world->steps[world->stepCount][1] = obj->currentCoords.y;
            world->stepCount++;
        }
        sObjectCells[i][0] = obj->currentCoords.x;
        sObjectCells[i][1] = obj->currentCoords.y;
        sObjectCellsValid[i] = true;
    }
}

static void UpdateParticles(struct Field3DFrame *frame)
{
    struct ParticleWorld world;
    uint32_t now = Harness_FrameCount();
    float dt;
    uint8_t weather;

    frame->particles = NULL;
    frame->particleCount = 0;
    if (!HdSettings_Get(HD_OPT_PARTICLES) || !frame->openAir)
    {
        Particles_Reset();
        sParticleLayoutId = -1;
        return;
    }
    if (sTable->layoutId != sParticleLayoutId)
    {
        Particles_Reset();
        memset(sObjectCellsValid, 0, sizeof(sObjectCellsValid));
        sParticleLayoutId = sTable->layoutId;
        sParticleFrame = now;
    }
    dt = (now - sParticleFrame) / 60.0f;
    sParticleFrame = now;

    weather = GetCurrentWeather();
    memset(&world, 0, sizeof(world));
    world.theme = sTable->particleTheme;
    world.center[0] = frame->target[0];
    world.center[1] = frame->target[2] - frame->target[1];
    world.rain = weather == WEATHER_RAIN || weather == WEATHER_RAIN_THUNDERSTORM || weather == WEATHER_DOWNPOUR;
    world.heavyRain = weather == WEATHER_RAIN_THUNDERSTORM || weather == WEATHER_DOWNPOUR;
    world.haze = weather == WEATHER_FOG_HORIZONTAL || weather == WEATHER_FOG_DIAGONAL || weather == WEATHER_VOLCANIC_ASH
              || weather == WEATHER_SANDSTORM || weather == WEATHER_ABNORMAL;
    world.sunny = frame->lighting == FIELD3D_LIGHT_OUTDOOR && !world.rain && !world.haze && weather != WEATHER_SHADE
               && !IsNight(frame->hour);
    world.wind = sinf(frame->time * 0.6f) * 0.6f + 0.3f;
    world.cellFlags = ParticleCellFlags;
    world.surface = ParticleSurface;
    CollectSteps(&world);
    frame->particleCount = Particles_Update(&world, dt, &frame->particles);
}

// Contact shadows go first in the particle list (drawn under the rest)
static void AddContactsToParticles(struct Field3DFrame *frame)
{
    int count = frame->particleCount;

    if (sContactCount == 0)
        return;
    if (count > FIELD3D_MAX_PARTICLES - sContactCount)
        count = FIELD3D_MAX_PARTICLES - sContactCount;
    memcpy(sDrawParticles, sContacts, sContactCount * sizeof(sContacts[0]));
    if (count > 0)
        memcpy(&sDrawParticles[sContactCount], frame->particles, count * sizeof(sContacts[0]));
    frame->particles = sDrawParticles;
    frame->particleCount = sContactCount + count;
}

bool Field3D_Prepare(struct Field3DFrame *frame, uint8_t *skipOam)
{
    int layoutId, mapId;
    bool meshRebuilt = false;
    float playerLevel, roofLevel;

    frame->active = false;
    frame->patchCount = 0;
    if (!Field3D_IsEnabled() || sDataDir[0] == '\0' || !InField() || !sSnapshot.valid)
        return false;

    sFrames++;
    layoutId = gMapHeader.mapLayoutId;
    mapId = (gSaveBlock1Ptr->location.mapGroup << 8) | gSaveBlock1Ptr->location.mapNum;
    if (sTable == NULL || sTable->layoutId != layoutId || TableChanged(layoutId))
    {
        bool changed = sTable != NULL && sTable->layoutId == layoutId;
        if (changed)
            printf("field3d: table for layout %d changed, reloading\n", layoutId);
        // the new map was usually a neighbor of the last one: already read
        sTable = NULL;
        sTable = GetTable(layoutId, changed);
        if (sTable == NULL)
        {
            static int sReported = -1;
            if (sReported != layoutId)
                printf("field3d: no table for layout %d, staying 2D\n", layoutId);
            sReported = layoutId;
            sNeighborsMap = -1;
            return false;
        }
        sMetatileLayout = NULL;
        sGridWidth = sGridHeight = 0; // force a rebuild
    }
    if (sNeighborsMap != mapId)
    {
        FindNeighbors();
        LoadNeighborTables();
        meshRebuilt = true;
    }
    if (sMetatileLayout != gMapHeader.mapLayout)
        CopyMetatileEntries(gMapHeader.mapLayout);
    if (SyncGrid())
        meshRebuilt = true;
    if (sMeshGrass != HdSettings_Get(HD_OPT_GRASS))
    {
        sMeshGrass = HdSettings_Get(HD_OPT_GRASS);
        meshRebuilt = true;
    }
    if (sDoorsChanged)
    {
        sDoorsChanged = false;
        CopyDoorEntries();
        if (!meshRebuilt)
            PatchDoors(frame);
    }
    if (meshRebuilt)
        BuildMesh();

    // The camera rises and moves south with the player's level (smoothly)
    playerLevel = PersonLevel(gSaveBlock1Ptr->pos.x + MAP_OFFSET + 0.5f, gSaveBlock1Ptr->pos.y + MAP_OFFSET + 0.5f,
                              gObjectEvents[gPlayerAvatar.objectEventId].currentElevation, &roofLevel);
    if (meshRebuilt && sTable->layoutId != sCameraLayoutId)
        sCameraLevel = playerLevel;
    sCameraLayoutId = sTable->layoutId;
    sCameraLevel += (playerLevel - sCameraLevel) * 0.12f;

    frame->active = true;
    frame->target[0] = (sSnapshot.originX + DISPLAY_WIDTH / 2) / 16.0f;
    frame->target[1] = sCameraLevel;
    frame->target[2] = (sSnapshot.originY + 88) / 16.0f + sCameraLevel;
    frame->scroll[0] = sSnapshot.scroll[0] & 0x1FF;
    frame->scroll[1] = sSnapshot.scroll[1] & 0x1FF;
    frame->mapMin[0] = MAP_OFFSET;
    frame->mapMin[1] = MAP_OFFSET;
    frame->mapMax[0] = MAP_OFFSET + sTable->width;
    frame->mapMax[1] = MAP_OFFSET + sTable->height;
    frame->time = sFrames / 60.0f;
    frame->lighting = LightingForMap();
    frame->openAir = OpenAir();
    frame->dim = DimForWeather();
    frame->hour = HdSettings_Get(HD_OPT_DAYNIGHT) ? GameHour() : -1.0f;
    PaletteFade(frame->fade);
    frame->actorCount = 0;
    frame->vertices = sVertices;
    frame->vertexCount = sVertexCount;
    frame->meshVersion = sMeshVersion;
    frame->patchVersion = sPatchVersion;
    frame->metatileEntries = sMetatileEntries;
    frame->metatileVersion = sMetatileVersion;
    CollectBillboards(frame, skipOam, HdSettings_Get(HD_OPT_LIGHTING) && HdSettings_Get(HD_OPT_SHADOWS),
                      HdSettings_GetValue(HD_OPT_STYLE) == HD_STYLE_PAPER);
    UpdateParticles(frame);
    AddContactsToParticles(frame);
    return true;
}

#endif // PORTABLE
