#ifdef PORTABLE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "battle_setup.h"
#include "main.h"
#include "overworld.h"
#include "field_camera.h"
#include "field_screen_effect.h"
#include "field_weather.h"
#include "field_player_avatar.h"
#include "intro.h"
#include "new_game.h"
#include "play_time.h"
#include "constants/maps.h"
#include "pokemon.h"
#include "script_pokemon_util.h"
#include "constants/items.h"
#include "constants/pokemon.h"
#include "constants/species.h"
#include "platform/test_hooks.h"

// PE_TEST_BATTLE="species,level[,shiny]"  wild battle against that species
// PE_TEST_PARTY="species,level"           Pokémon given to the player first (battles), and
//                                         right away with PE_TEST_QUICKSTART
//                                         (default: Treecko lv 5, if the party is empty)
// PE_TEST_BATTLE_FRAME=N                  first frame to try (default 0); the
//                                         battle starts once the field is running
// Species are internal numbers (include/constants/species.h).
// PE_TEST_CAMERA_AT=a,b,c                 print the field camera origin at these frames
// PE_TEST_WARP="map,x,y"                  warp there (map = group << 8 | number, as in
//                                         include/constants/map_groups.h) once the field
//                                         runs, from PE_TEST_WARP_FRAME on
// PE_TEST_QUICKSTART=1                    skip the intro: a new game starts right on the
//                                         PE_TEST_WARP map (Littleroot if none) once the
//                                         copyright screen is over
// PE_TEST_TOUR=file                       visit maps one after another (needs the field
//                                         running, e.g. with PE_TEST_QUICKSTART). Lines:
//                                         "map x y name [what]". Once each map has settled
//                                         the renderer saves an overview picture <name>.png
//                                         in PE_DUMP_DIR; the run ends after the last map.
//                                         what: o (overview, default), g (game screen,
//                                         <name>_game.png) or og (both)
// PE_TEST_TOUR_SETTLE=N                   frames to wait on each map (default 90)
// PE_TEST_WEATHER=N                       keep this weather on the field (include/constants/weather.h)
// PE_TEST_AVATAR=N                        player avatar flags once the field runs
//                                         (2 mach bike, 4 acro bike, 8 surfing)
// PE_TEST_AVATAR_OFF=N                    back on foot at frame N
// PE_TEST_TOUR_PPB=N                      overview pixels per block (default 16, as the GBA)
// PE_TEST_TOUR_SHOTS=1                    also save the game screen as <name>_game.png

static bool sPending;
static uint32_t sStartFrame;
static int sSpecies, sLevel, sShiny;
static int sPartySpecies = SPECIES_TREECKO, sPartyLevel = 5;
static bool sWarpPending;
static uint32_t sWarpFrame;
static int sWarpMap, sWarpX, sWarpY;
static bool sQuickStart;
static int sWeather = -1;
static int sAvatar;
static uint32_t sAvatarOff;

struct TourStop
{
    int map, x, y;
    char name[96];
    bool overview, game;
};

enum { TOUR_OFF, TOUR_WARP, TOUR_SETTLE, TOUR_SHOOT, TOUR_DONE };

static struct TourStop *sTour;
static int sTourCount, sTourIndex, sTourState = TOUR_OFF;
static uint32_t sTourSettle = 90, sTourWait;
static int sTourPixelsPerBlock = 16;
static bool sTourShots;

static void LoadTour(const char *path)
{
    char line[256];
    FILE *file = fopen(path, "r");
    int capacity = 0;

    if (file == NULL)
    {
        fprintf(stderr, "test: cannot open tour file %s\n", path);
        return;
    }
    while (fgets(line, sizeof(line), file) != NULL)
    {
        struct TourStop stop;
        char what[8] = "";
        if (line[0] == '#' || sscanf(line, "%d %d %d %95s %7s", &stop.map, &stop.x, &stop.y, stop.name, what) < 4)
            continue;
        stop.overview = what[0] == '\0' || strchr(what, 'o') != NULL;
        stop.game = strchr(what, 'g') != NULL;
        if (sTourCount == capacity)
        {
            capacity = capacity ? capacity * 2 : 64;
            sTour = realloc(sTour, capacity * sizeof(*sTour));
        }
        sTour[sTourCount++] = stop;
    }
    fclose(file);
    if (sTourCount > 0)
        sTourState = TOUR_WARP;
}

void TestHooks_Init(void)
{
    const char *value;

    if ((value = getenv("PE_TEST_BATTLE")) != NULL)
    {
        sLevel = 5;
        if (sscanf(value, "%d,%d,%d", &sSpecies, &sLevel, &sShiny) >= 1 && sSpecies > 0 && sSpecies < NUM_SPECIES)
            sPending = true;
        else
            fprintf(stderr, "test: bad PE_TEST_BATTLE '%s'\n", value);
    }
    if ((value = getenv("PE_TEST_PARTY")) != NULL)
        sscanf(value, "%d,%d", &sPartySpecies, &sPartyLevel);
    if ((value = getenv("PE_TEST_BATTLE_FRAME")) != NULL)
        sStartFrame = strtoul(value, NULL, 10);
    if ((value = getenv("PE_TEST_WARP")) != NULL)
    {
        if (sscanf(value, "%d,%d,%d", &sWarpMap, &sWarpX, &sWarpY) == 3)
            sWarpPending = true;
        else
            fprintf(stderr, "test: bad PE_TEST_WARP '%s'\n", value);
    }
    if ((value = getenv("PE_TEST_WARP_FRAME")) != NULL)
        sWarpFrame = strtoul(value, NULL, 10);
    if ((value = getenv("PE_TEST_QUICKSTART")) != NULL && value[0] == '1')
        sQuickStart = true;
    if ((value = getenv("PE_TEST_WEATHER")) != NULL)
        sWeather = atoi(value);
    if ((value = getenv("PE_TEST_AVATAR")) != NULL)
        sAvatar = atoi(value);
    if ((value = getenv("PE_TEST_AVATAR_OFF")) != NULL)
        sAvatarOff = strtoul(value, NULL, 10);
    if ((value = getenv("PE_TEST_TOUR_SETTLE")) != NULL)
        sTourSettle = strtoul(value, NULL, 10);
    if ((value = getenv("PE_TEST_TOUR_PPB")) != NULL && atoi(value) > 0)
        sTourPixelsPerBlock = atoi(value);
    if ((value = getenv("PE_TEST_TOUR_SHOTS")) != NULL && value[0] == '1')
        sTourShots = true;
    if ((value = getenv("PE_TEST_TOUR")) != NULL)
        LoadTour(value);
}

// The last steps of a normal warp (Task_WarpAndLoadMap), without the fade out
static void WarpNow(int map, int x, int y)
{
    SetWarpDestination(MAP_GROUP(map), MAP_NUM(map), WARP_ID_NONE, x, y);
    WarpIntoMap();
    SetMainCallback2(CB2_LoadMap);
}

// What CB2_NewGame does, minus the truck: the map loads like after a warp
static void QuickStart(void)
{
    static const u8 sName[] = {0xC2, 0xBE, 0xFF}; // "HD"

    NewGameInitData();
    memcpy(gSaveBlock2Ptr->playerName, sName, sizeof(sName));
    gSaveBlock2Ptr->playerGender = MALE;
    ResetInitialPlayerAvatarState();
    PlayTimeCounter_Start();
    if (getenv("PE_TEST_PARTY") != NULL)
    {
        ZeroPlayerPartyMons();
        ScriptGiveMon(sPartySpecies, sPartyLevel, ITEM_NONE, 0, 0, 0);
    }
    if (sWarpPending)
    {
        sWarpPending = false;
        WarpNow(sWarpMap, sWarpX, sWarpY);
    }
    else
    {
        WarpNow(MAP_LITTLEROOT_TOWN, 10, 10);
    }
    printf("test: quick start\n");
}

static void RunTour(void)
{
    const struct TourStop *stop;

    if (sTourState == TOUR_OFF || sTourState == TOUR_DONE || gMain.callback2 != CB2_Overworld)
        return;
    stop = &sTour[sTourIndex];
    switch (sTourState)
    {
    case TOUR_WARP:
        WarpNow(stop->map, stop->x, stop->y);
        sTourWait = 0;
        sTourState = TOUR_SETTLE;
        break;
    case TOUR_SETTLE:
        if (++sTourWait < sTourSettle)
            break;
        if (gSaveBlock1Ptr->location.mapGroup != MAP_GROUP(stop->map) || gSaveBlock1Ptr->location.mapNum != MAP_NUM(stop->map))
            printf("test: tour %s: ended up on map %d.%d\n", stop->name, gSaveBlock1Ptr->location.mapGroup,
                   gSaveBlock1Ptr->location.mapNum);
        sTourState = TOUR_SHOOT; // the renderer takes the picture during this frame
        break;
    case TOUR_SHOOT:
        sTourIndex++;
        sTourState = sTourIndex < sTourCount ? TOUR_WARP : TOUR_DONE;
        break;
    }
}

const char *TestHooks_OverviewWanted(void)
{
    return sTourState == TOUR_SHOOT && sTour[sTourIndex].overview ? sTour[sTourIndex].name : NULL;
}

const char *TestHooks_GameShotWanted(void)
{
    if (sTourState != TOUR_SHOOT)
        return NULL;
    return (sTour[sTourIndex].game || sTourShots) ? sTour[sTourIndex].name : NULL;
}

bool TestHooks_TourShots(void)
{
    return sTourShots;
}

int TestHooks_OverviewPixelsPerBlock(void)
{
    return sTourPixelsPerBlock;
}

bool TestHooks_Finished(void)
{
    return sTourState == TOUR_DONE;
}

static void WarpIfAsked(uint32_t frame)
{
    if (!sWarpPending || frame < sWarpFrame || gMain.callback2 != CB2_Overworld)
        return;
    sWarpPending = false;
    SetWarpDestination(MAP_GROUP(sWarpMap), MAP_NUM(sWarpMap), WARP_ID_NONE, sWarpX, sWarpY);
    DoWarp();
    printf("test: warp to map %d.%d (%d,%d) at frame %u\n", MAP_GROUP(sWarpMap), MAP_NUM(sWarpMap), sWarpX, sWarpY, frame);
}

static void PrintCameraIfAsked(uint32_t frame)
{
    const char *list = getenv("PE_TEST_CAMERA_AT");
    char key[16];
    size_t len;
    const char *p;

    if (list == NULL || gMain.callback2 != CB2_Overworld)
        return;
    snprintf(key, sizeof(key), "%u", frame);
    len = strlen(key);
    for (p = strstr(list, key); p != NULL; p = strstr(p + 1, key))
    {
        if ((p == list || p[-1] == ',') && (p[len] == '\0' || p[len] == ','))
        {
            s32 x, y;
            FieldCamera_GetScreenOrigin(&x, &y);
            printf("test: frame %u camera origin %d,%d pos %d,%d\n", frame, x, y,
                   gSaveBlock1Ptr->pos.x, gSaveBlock1Ptr->pos.y);
            return;
        }
    }
}

void TestHooks_OnFrame(uint32_t frame)
{
    // after the copyright screen, which loads the save
    if (sQuickStart && gMain.callback2 != CB2_InitCopyrightScreenAfterBootup && gSaveBlock2Ptr != NULL)
    {
        sQuickStart = false;
        QuickStart();
    }
    PrintCameraIfAsked(frame);
    WarpIfAsked(frame);
    RunTour();
    if (sWeather >= 0 && gMain.callback2 == CB2_Overworld && GetCurrentWeather() != sWeather)
        SetCurrentAndNextWeatherNoDelay(sWeather);
    if (sAvatar != 0 && !sQuickStart && !sWarpPending && gMain.callback2 == CB2_Overworld && frame > 200)
    {
        SetPlayerAvatarTransitionFlags(sAvatar);
        sAvatar = 0;
    }
    if (sAvatarOff != 0 && frame >= sAvatarOff && gMain.callback2 == CB2_Overworld)
    {
        SetPlayerAvatarTransitionFlags(PLAYER_AVATAR_FLAG_ON_FOOT);
        sAvatarOff = 0;
    }
    if (!sPending || frame < sStartFrame || gMain.callback2 != CB2_Overworld)
        return;
    sPending = false;

    if (CalculatePlayerPartyCount() == 0 || getenv("PE_TEST_PARTY") != NULL)
    {
        ZeroPlayerPartyMons();
        ScriptGiveMon(sPartySpecies, sPartyLevel, ITEM_NONE, 0, 0, 0);
    }
    CreateScriptedWildMon(sSpecies, sLevel, ITEM_NONE);
    if (sShiny)
    {
        // Personality 0 with OT ID 0 always makes a shiny Pokémon
        CreateMon(&gEnemyParty[0], sSpecies, sLevel, USE_RANDOM_IVS, TRUE, 0, OT_ID_PRESET, 0);
    }
    BattleSetup_StartScriptedWildBattle();
    printf("test: wild battle vs species %d lv %d%s at frame %u\n", sSpecies, sLevel, sShiny ? " (shiny)" : "", frame);
}

#endif // PORTABLE
