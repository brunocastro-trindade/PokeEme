#ifndef GUARD_PLATFORM_HD_SETTINGS_H
#define GUARD_PLATFORM_HD_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

// What the HD-2D layer draws. The in-game menu (hd_menu.c) changes these;
// they are kept in a small text file next to the save.

enum
{
    HD_OPT_FIELD3D,    // 3D field (else the GBA's 2D map)
    HD_OPT_ZOOM,       // camera distance in the 3D field: HD_ZOOM_*
    HD_OPT_STYLE,      // how people look in the 3D field: HD_STYLE_*
    HD_OPT_HDSPRITES,  // animated Black/White Pokémon sprites
    HD_OPT_LIGHTING,   // sun and ambient light
    HD_OPT_DAYNIGHT,   // the outdoor light follows the game's clock
    HD_OPT_SHADOWS,    // sun shadows
    HD_OPT_DOF,        // depth of field (tilt-shift blur)
    HD_OPT_BLOOM,      // glow around bright areas
    HD_OPT_GRADING,    // haze, color and vignette
    HD_OPT_GRASS,      // standing, swaying tall grass
    HD_OPT_PARTICLES,  // particle effects
    HD_OPT_FOLLOWER,   // the first party Pokémon walks behind the player (src/follower.c)
    HD_OPT_FPS,        // frame counter in a corner
    HD_OPT_COUNT,
};

enum
{
    HD_STYLE_HD2D,  // sprites as they are
    HD_STYLE_PAPER, // paper figures: white die-cut rim, a stand, rocking as they walk
    HD_STYLE_COUNT,
};

enum
{
    HD_ZOOM_FAR,
    HD_ZOOM_NORMAL,
    HD_ZOOM_NEAR,
    HD_ZOOM_COUNT,
};

struct HdOptionInfo
{
    const char *key;   // name in the settings file and in PE_HD
    const char *label; // menu text (UTF-8)
    uint8_t defaultValue;
    uint8_t valueCount;              // 0: on/off
    const char *const *valueNames;   // menu text of each value (NULL: "Não" / "Sim")
};

extern const struct HdOptionInfo gHdOptionInfo[HD_OPT_COUNT];

// Loads the file (if path is not NULL), then applies the environment:
//   PE_HD="dof=0,particles=1"   any option by key
//   PE_FX=0                     lighting, shadows, blur, bloom, grading, particles off
//   PE_DOF=0, PE_PARTICLES=0    one option off
//   PE_FIELD3D=0, PE_HDMON=0    (read by the harness, mirrored here)
void HdSettings_Init(const char *path);

// On/off options; for the others, true when not at their first value
bool HdSettings_Get(int option);
void HdSettings_Set(int option, bool value);
void HdSettings_Toggle(int option); // next value (after the last, the first)
int HdSettings_GetValue(int option);
void HdSettings_SetValue(int option, int value);
void HdSettings_Step(int option, int delta); // previous / next value, stops at the ends
int HdSettings_ValueCount(int option);

// Changes every time an option changes (renderers cache what depends on them)
uint32_t HdSettings_Version(void);

// Why an option cannot work (its data was not found, or is from an older
// build), shown in the menu instead of its value; NULL when it is fine
void HdSettings_SetProblem(int option, const char *text);
const char *HdSettings_GetProblem(int option);

// Writes the file if something changed since the last save (no-op without a path)
void HdSettings_Save(void);

#endif // GUARD_PLATFORM_HD_SETTINGS_H
