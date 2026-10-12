#ifdef PORTABLE
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "event_scripts.h"
#include "field_player_avatar.h"
#include "field_weather.h"
#include "fieldmap.h"
#include "follower.h"
#include "metatile_behavior.h"
#include "main.h"
#include "overworld.h"
#include "palette.h"
#include "pokemon.h"
#include "random.h"
#include "sprite.h"
#include "string_util.h"
#include "trainer_hill.h"
#include "constants/battle.h"
#include "constants/event_object_movement.h"
#include "constants/event_objects.h"
#include "constants/map_types.h"
#include "constants/region_map_sections.h"
#include "constants/weather.h"
#include "platform/follower_gfx.h"
#include "platform/hd_settings.h"

#define FOLLOWER_PAL_TAG 0x1130 // not in the object event palette table: loaded from the sprite data
#define PAL_TAG_NONE     0x11FF // OBJ_EVENT_PAL_TAG_NONE (event_object_movement.c)
#define QUEUE_SIZE       4

enum { SPEED_SLOW, SPEED_NORMAL, SPEED_FAST, SPEED_FASTER };

struct FollowerStep
{
    s16 x, y;  // where to go: the tile the player just left
    u8 speed;
};

struct FollowerState
{
    bool8 active;
    u8 objectEventId;
    int key;               // sprite key (species, or an Unown letter)
    bool8 shiny;
    s16 playerX, playerY;  // the player's tile last frame
    s16 leftX, leftY;      // the tile the player last stepped off
    bool8 waiting;         // hidden on the player's tile until the player takes a step
    bool8 hidden;          // in its ball (bike, surf, the player hidden) last frame
    struct FollowerStep queue[QUEUE_SIZE];
    u8 queueCount;
};

static struct FollowerState sFollower;
static struct ObjectEventGraphicsInfo sGraphicsInfo;
static struct SpriteFrameImage sImages[FOLLOWER_GFX_FRAMES];

void InitStandardObjectEventGraphicsInfo32x32(struct ObjectEventGraphicsInfo *info); // event_object_movement.c

// --- what follows -------------------------------------------------------------

static struct Pokemon *Lead(void)
{
    struct Pokemon *mon = &gPlayerParty[0];
    u16 species = GetMonData(mon, MON_DATA_SPECIES_OR_EGG);

    if (species == SPECIES_NONE || species == SPECIES_EGG || species >= NUM_SPECIES)
        return NULL;
    if (GetMonData(mon, MON_DATA_HP) == 0)
        return NULL;
    return mon;
}

// The sprite to use: the species, or the Unown letter
static int SpriteKey(struct Pokemon *mon)
{
    u16 species = GetMonData(mon, MON_DATA_SPECIES);

    if (species == SPECIES_UNOWN)
    {
        u8 letter = GET_UNOWN_LETTER(GetMonData(mon, MON_DATA_PERSONALITY));
        if (letter > 0)
            return SPECIES_UNOWN_B + letter - 1;
    }
    return species;
}

// Places where it would get in the way or where the party is not the player's
static bool8 MapAllowsFollower(void)
{
    if (IsOverworldLinkActive() || InTrainerHill())
        return FALSE;
    if (gMapHeader.regionMapSectionId == MAPSEC_BATTLE_FRONTIER && gMapHeader.mapType == MAP_TYPE_INDOOR)
        return FALSE;
    return TRUE;
}

static struct ObjectEvent *Object(void)
{
    struct ObjectEvent *obj;

    if (!sFollower.active)
        return NULL;
    obj = &gObjectEvents[sFollower.objectEventId];
    if (!obj->active || !Follower_IsFollower(obj))
    {
        sFollower.active = FALSE; // gone with the map (warp) or removed by the game
        return NULL;
    }
    return obj;
}

bool8 Follower_IsFollower(const struct ObjectEvent *objectEvent)
{
    return objectEvent->active && objectEvent->localId == LOCALID_FOLLOWER
        && objectEvent->graphicsId == OBJ_EVENT_GFX_FOLLOWER;
}

// --- graphics -------------------------------------------------------------------

const struct ObjectEventGraphicsInfo *Follower_GetGraphicsInfo(void)
{
    return &sGraphicsInfo;
}

// The follower's palette goes in slot 11, where a "special" object (rival,
// truck, movers...) keeps its reflection colors, while no such object is on
// the map. Slots 12-15 are left to the weather (two) and the field effects
// (grass and dust/footprints/splashes need one each); taking one of them made
// those effects run out of palettes and show with the wrong colors.
#define FIXED_SLOT PALSLOT_NPC_SPECIAL_REFLECTION

static bool8 SpecialObjectOnMap(void)
{
    int i;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        const struct ObjectEvent *obj = &gObjectEvents[i];
        if (obj->active && !Follower_IsFollower(obj)
         && GetObjectEventGraphicsInfo(obj->graphicsId)->paletteSlot == PALSLOT_NPC_SPECIAL)
            return TRUE;
    }
    return FALSE;
}

// Puts the colors in a palette slot (once; again if something overwrote
// them); returns the slot, 0xFF if none is free
static u8 PlacePalette(const u16 *colors)
{
    struct SpritePalette palette;
    u8 slot;

    // slot 11 is only reserved for objects while 12 are (not in link rooms)
    if (gReservedSpritePaletteCount > FIXED_SLOT && !SpecialObjectOnMap())
    {
        FreeSpritePaletteByTag(FOLLOWER_PAL_TAG);
        if (memcmp(&gPlttBufferUnfaded[OBJ_PLTT_ID(FIXED_SLOT)], colors, PLTT_SIZE_4BPP) != 0)
        {
            LoadPalette(colors, OBJ_PLTT_ID(FIXED_SLOT), PLTT_SIZE_4BPP);
            UpdateSpritePaletteWithWeather(FIXED_SLOT);
        }
        return FIXED_SLOT;
    }
    slot = IndexOfSpritePaletteTag(FOLLOWER_PAL_TAG);
    if (slot != 0xFF)
        return slot;
    palette.data = colors;
    palette.tag = FOLLOWER_PAL_TAG;
    slot = LoadSpritePalette(&palette);
    if (slot != 0xFF)
        UpdateSpritePaletteWithWeather(slot);
    return slot;
}

// Loads the frames and the palette of a sprite key; FALSE if it cannot be shown
static bool8 PrepareGraphics(int key, bool8 shiny)
{
    const u16 *colors;
    const u8 *frames = FollowerGfx_Get(key, shiny, &colors);
    u8 slot;
    int i;

    if (frames == NULL)
        return FALSE;
    FreeSpritePaletteByTag(FOLLOWER_PAL_TAG);
    slot = PlacePalette(colors);
    if (slot == 0xFF)
        return FALSE; // every sprite palette is taken

    for (i = 0; i < FOLLOWER_GFX_FRAMES; i++)
    {
        sImages[i].data = frames + i * FOLLOWER_GFX_FRAME_SIZE;
        sImages[i].size = FOLLOWER_GFX_FRAME_SIZE;
    }
    InitStandardObjectEventGraphicsInfo32x32(&sGraphicsInfo);
    sGraphicsInfo.tileTag = TAG_NONE;
    sGraphicsInfo.paletteTag = FOLLOWER_PAL_TAG;
    sGraphicsInfo.reflectionPaletteTag = PAL_TAG_NONE;
    sGraphicsInfo.size = FOLLOWER_GFX_FRAME_SIZE;
    sGraphicsInfo.width = 32;
    sGraphicsInfo.height = 32;
    sGraphicsInfo.paletteSlot = slot;
    sGraphicsInfo.shadowSize = SHADOW_SIZE_M;
    sGraphicsInfo.inanimate = FALSE;
    sGraphicsInfo.disableReflectionPaletteLoad = TRUE;
    sGraphicsInfo.tracks = TRACKS_FOOT;
    sGraphicsInfo.images = sImages;
    return TRUE;
}

// Every frame: menus and battles reset the sprite palettes, special objects
// come and go
static void KeepPalette(struct ObjectEvent *obj)
{
    const u16 *colors;
    u8 slot;

    if (FollowerGfx_Get(sFollower.key, sFollower.shiny, &colors) == NULL)
        return;
    slot = PlacePalette(colors);
    if (slot == 0xFF)
        return;
    sGraphicsInfo.paletteSlot = slot;
    gSprites[obj->spriteId].oam.paletteNum = slot;
}

// --- spawning ---------------------------------------------------------------------

static bool8 Spawn(s16 x, s16 y, u8 direction, bool8 waiting)
{
    struct ObjectEvent *player = &gObjectEvents[gPlayerAvatar.objectEventId];
    struct Pokemon *mon = Lead();
    struct ObjectEvent *obj;
    u8 id;

    if (mon == NULL)
        return FALSE;
    sFollower.key = SpriteKey(mon);
    sFollower.shiny = IsMonShiny(mon);
    if (!PrepareGraphics(sFollower.key, sFollower.shiny))
        return FALSE;
    id = SpawnSpecialObjectEventParameterized(OBJ_EVENT_GFX_FOLLOWER, MOVEMENT_TYPE_NONE, LOCALID_FOLLOWER, x, y,
                                              player->currentElevation);
    if (id >= OBJECT_EVENTS_COUNT)
    {
        FreeSpritePaletteByTag(FOLLOWER_PAL_TAG);
        return FALSE;
    }
    obj = &gObjectEvents[id];
    sFollower.active = TRUE;
    sFollower.objectEventId = id;
    sFollower.waiting = waiting;
    sFollower.queueCount = 0;
    obj->invisible = waiting;
    gSprites[obj->spriteId].invisible = waiting;
    obj->previousElevation = player->previousElevation;
    ObjectEventTurn(obj, direction);
    return TRUE;
}

bool8 Follower_Remove(void)
{
    struct ObjectEvent *obj = Object();

    sFollower.active = FALSE;
    sFollower.queueCount = 0;
    if (obj == NULL)
        return FALSE;
    RemoveObjectEventByLocalIdAndMap(LOCALID_FOLLOWER, obj->mapNum, obj->mapGroup);
    FreeSpritePaletteByTag(FOLLOWER_PAL_TAG);
    return TRUE;
}

// --- moving -----------------------------------------------------------------------

// How fast the player is going, from the movement it is doing
static u8 PlayerSpeed(void)
{
    u8 action = gObjectEvents[gPlayerAvatar.objectEventId].movementActionId;

#define IN(first) (action >= (first) && action < (first) + 4)
    if (IN(MOVEMENT_ACTION_WALK_SLOW_DOWN))
        return SPEED_SLOW;
    if (IN(MOVEMENT_ACTION_WALK_FAST_DOWN) || IN(MOVEMENT_ACTION_PLAYER_RUN_DOWN) || IN(MOVEMENT_ACTION_SLIDE_DOWN)
     || IN(MOVEMENT_ACTION_RIDE_WATER_CURRENT_DOWN))
        return SPEED_FAST;
    if (IN(MOVEMENT_ACTION_WALK_FASTER_DOWN))
        return SPEED_FASTER;
#undef IN
    return SPEED_NORMAL;
}

static u8 DirectionTo(s16 dx, s16 dy)
{
    if (dx > 0)
        return DIR_EAST;
    if (dx < 0)
        return DIR_WEST;
    if (dy > 0)
        return DIR_SOUTH;
    return DIR_NORTH;
}

static u8 WalkAction(u8 direction, u8 speed)
{
    switch (speed)
    {
    case SPEED_SLOW:   return GetWalkSlowMovementAction(direction);
    case SPEED_FAST:   return GetWalkFastMovementAction(direction);
    case SPEED_FASTER: return GetWalkFasterMovementAction(direction);
    default:           return GetWalkNormalMovementAction(direction);
    }
}

static bool8 IsBusy(struct ObjectEvent *obj)
{
    if (!obj->heldMovementActive)
        return FALSE;
    if (ObjectEventClearHeldMovementIfFinished(obj))
        return FALSE;
    return TRUE;
}

// Starts the next queued step if the follower is free
static void RunQueue(struct ObjectEvent *obj)
{
    struct FollowerStep step;
    s16 dx, dy;
    u8 direction;

    if (sFollower.queueCount == 0 || IsBusy(obj))
        return;
    step = sFollower.queue[0];
    memmove(&sFollower.queue[0], &sFollower.queue[1], (sFollower.queueCount - 1) * sizeof(step));
    sFollower.queueCount--;

    dx = step.x - obj->currentCoords.x;
    dy = step.y - obj->currentCoords.y;
    if (sFollower.waiting)
    {
        // it comes out where the player was
        if (dx != 0 || dy != 0)
            MoveObjectEventToMapCoords(obj, step.x, step.y);
        obj->invisible = FALSE;
        sFollower.waiting = FALSE;
        direction = DirectionTo(gObjectEvents[gPlayerAvatar.objectEventId].currentCoords.x - step.x,
                                gObjectEvents[gPlayerAvatar.objectEventId].currentCoords.y - step.y);
        ObjectEventTurn(obj, direction);
        return;
    }
    if (dx == 0 && dy == 0)
        return;
    direction = DirectionTo(dx, dy);
    if (abs(dx) + abs(dy) == 1)
        ObjectEventSetHeldMovement(obj, WalkAction(direction, step.speed));
    else if ((abs(dx) == 2 && dy == 0) || (dx == 0 && abs(dy) == 2))
        ObjectEventSetHeldMovement(obj, GetJump2MovementAction(direction)); // over a ledge, after the player
    else
    {
        MoveObjectEventToMapCoords(obj, step.x, step.y); // too far (warped, pushed by a script)
        ObjectEventTurn(obj, direction);
    }
}

static void Enqueue(s16 x, s16 y, u8 speed)
{
    if (sFollower.queueCount == QUEUE_SIZE)
    {
        // falling behind: drop the oldest steps, the follower jumps ahead
        memmove(&sFollower.queue[0], &sFollower.queue[1], (QUEUE_SIZE - 1) * sizeof(sFollower.queue[0]));
        sFollower.queueCount--;
    }
    sFollower.queue[sFollower.queueCount].x = x;
    sFollower.queue[sFollower.queueCount].y = y;
    sFollower.queue[sFollower.queueCount].speed = speed;
    sFollower.queueCount++;
}

// Appears on the tile the player just left, if it is dry ground
static void ComeOut(struct ObjectEvent *obj, struct ObjectEvent *player)
{
    s16 x = sFollower.leftX, y = sFollower.leftY;

    if (abs(x - player->currentCoords.x) + abs(y - player->currentCoords.y) != 1)
        return;
    if (MapGridGetCollisionAt(x, y) != 0 || MetatileBehavior_IsSurfableWaterOrUnderwater(MapGridGetMetatileBehaviorAt(x, y)))
        return;
    Enqueue(x, y, SPEED_NORMAL);
}

// On the bike, surfing, diving, flying or in a cutscene that hides the player
static bool8 ShouldHide(struct ObjectEvent *player)
{
    if (TestPlayerAvatarFlags(PLAYER_AVATAR_FLAG_MACH_BIKE | PLAYER_AVATAR_FLAG_ACRO_BIKE | PLAYER_AVATAR_FLAG_SURFING
                              | PLAYER_AVATAR_FLAG_UNDERWATER))
        return TRUE;
    return player->invisible || gSprites[player->spriteId].invisible;
}

void Follower_Update(void)
{
    struct ObjectEvent *player, *obj;
    struct Pokemon *mon;

    if (!HdSettings_Get(HD_OPT_FOLLOWER) || gMain.callback2 != CB2_Overworld || !MapAllowsFollower())
    {
        // during battle transitions (CB2_OverworldBasic) it just stays
        if (gMain.callback2 == CB2_Overworld || !HdSettings_Get(HD_OPT_FOLLOWER))
            Follower_Remove();
        return;
    }
    player = &gObjectEvents[gPlayerAvatar.objectEventId];
    mon = Lead();
    obj = Object();

    // a new lead (party order, evolution, fainted): swap it in place
    if (obj != NULL && (mon == NULL || SpriteKey(mon) != sFollower.key || IsMonShiny(mon) != sFollower.shiny))
    {
        s16 x = obj->currentCoords.x, y = obj->currentCoords.y;
        u8 direction = obj->facingDirection;
        bool8 waiting = sFollower.waiting;
        Follower_Remove();
        obj = NULL;
        if (mon != NULL && Spawn(x, y, direction, waiting))
            obj = Object();
    }
    if (mon == NULL)
        return;
    if (obj == NULL)
    {
        // new map (or just turned on): it waits on the player's tile
        if (!Spawn(player->currentCoords.x, player->currentCoords.y, player->facingDirection, TRUE))
            return;
        obj = Object();
        sFollower.playerX = sFollower.leftX = player->currentCoords.x;
        sFollower.playerY = sFollower.leftY = player->currentCoords.y;
    }
    KeepPalette(obj);

    if (player->currentCoords.x != sFollower.playerX || player->currentCoords.y != sFollower.playerY)
    {
        Enqueue(sFollower.playerX, sFollower.playerY, PlayerSpeed());
        sFollower.leftX = sFollower.playerX;
        sFollower.leftY = sFollower.playerY;
        sFollower.playerX = player->currentCoords.x;
        sFollower.playerY = player->currentCoords.y;
    }

    if (ShouldHide(player))
    {
        // in its ball: wait on the player's tile
        sFollower.hidden = TRUE;
        sFollower.queueCount = 0;
        if (!IsBusy(obj))
        {
            if (obj->currentCoords.x != player->currentCoords.x || obj->currentCoords.y != player->currentCoords.y)
                MoveObjectEventToMapCoords(obj, player->currentCoords.x, player->currentCoords.y);
            obj->currentElevation = player->currentElevation;
            obj->previousElevation = player->previousElevation;
            obj->invisible = TRUE;
            sFollower.waiting = TRUE;
        }
        return;
    }
    if (sFollower.hidden)
    {
        // out of a door, off the bike: it comes out right behind
        sFollower.hidden = FALSE;
        if (sFollower.waiting && sFollower.queueCount == 0)
            ComeOut(obj, player);
    }
    RunQueue(obj);
}

// --- talking to it ------------------------------------------------------------------

static const u8 sText_Poisoned[] = _("{STR_VAR_1} is shivering with the\neffects of being poisoned.");
static const u8 sText_Burned[] = _("{STR_VAR_1} is hurting from its burn.");
static const u8 sText_Paralyzed[] = _("{STR_VAR_1} is trying hard to keep\nup despite its paralysis.");
static const u8 sText_Asleep[] = _("{STR_VAR_1} seems very sleepy.");
static const u8 sText_Frozen[] = _("{STR_VAR_1} is shivering from the\ncold.");
static const u8 sText_Tired[] = _("{STR_VAR_1} looks very tired.");
static const u8 sText_RainWater[] = _("{STR_VAR_1} seems to be enjoying\nthe rain!");
static const u8 sText_Rain[] = _("{STR_VAR_1} is getting soaked by\nthe rain.");
static const u8 sText_Cave[] = _("{STR_VAR_1} is peering into the\ndarkness.");
static const u8 sText_Love[] = _("{STR_VAR_1} is gazing at you\nlovingly.");
static const u8 sText_VeryHappy[] = _("{STR_VAR_1} is very happy to be\nwith you!");
static const u8 sText_Happy[] = _("{STR_VAR_1} is happy.");
static const u8 sText_Cheerful[] = _("{STR_VAR_1} is walking along\ncheerfully.");
static const u8 sText_Calm[] = _("{STR_VAR_1} is looking around.");
static const u8 sText_Distant[] = _("{STR_VAR_1} is keeping its distance.");
static const u8 sText_Ignoring[] = _("{STR_VAR_1} seems to be ignoring\nyou.");

static const u8 *TalkText(struct Pokemon *mon)
{
    u16 species = GetMonData(mon, MON_DATA_SPECIES);
    u32 status = GetMonData(mon, MON_DATA_STATUS);
    u16 hp = GetMonData(mon, MON_DATA_HP), maxHp = GetMonData(mon, MON_DATA_MAX_HP);
    u8 friendship = GetMonData(mon, MON_DATA_FRIENDSHIP);
    u8 weather = GetCurrentWeather();
    bool8 rain = weather == WEATHER_RAIN || weather == WEATHER_RAIN_THUNDERSTORM || weather == WEATHER_DOWNPOUR;
    bool8 water = gSpeciesInfo[species].types[0] == TYPE_WATER || gSpeciesInfo[species].types[1] == TYPE_WATER;

    if (status & STATUS1_PSN_ANY)
        return sText_Poisoned;
    if (status & STATUS1_BURN)
        return sText_Burned;
    if (status & STATUS1_PARALYSIS)
        return sText_Paralyzed;
    if (status & STATUS1_SLEEP)
        return sText_Asleep;
    if (status & STATUS1_FREEZE)
        return sText_Frozen;
    if (maxHp != 0 && hp * 4 < maxHp)
        return sText_Tired;
    // now and then, a word about the place
    if ((Random() % 3) == 0)
    {
        if (rain)
            return water ? sText_RainWater : sText_Rain;
        if (gMapHeader.mapType == MAP_TYPE_UNDERGROUND)
            return sText_Cave;
    }
    if (friendship >= 255)
        return sText_Love;
    if (friendship >= 200)
        return sText_VeryHappy;
    if (friendship >= 150)
        return sText_Happy;
    if (friendship >= 100)
        return sText_Cheerful;
    if (friendship >= 50)
        return sText_Calm;
    if (friendship >= 20)
        return sText_Distant;
    return sText_Ignoring;
}

void Follower_PrepareTalk(void)
{
    struct Pokemon *mon = Lead();
    u8 nickname[POKEMON_NAME_LENGTH + 1];

    if (mon == NULL)
        mon = &gPlayerParty[0];
    gSpecialVar_0x8004 = GetMonData(mon, MON_DATA_SPECIES);
    GetMonData(mon, MON_DATA_NICKNAME, nickname);
    StringCopy_Nickname(gStringVar1, nickname);
    StringExpandPlaceholders(gStringVar4, TalkText(mon));
}

const u8 *Follower_GetScript(void)
{
    return Follower_EventScript_Talk;
}

#endif // PORTABLE
