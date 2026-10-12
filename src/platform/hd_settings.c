#ifdef PORTABLE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/hd_settings.h"

static const char *const sZoomNames[HD_ZOOM_COUNT] = {"Longe", "Normal", "Perto"};
static const char *const sStyleNames[HD_STYLE_COUNT] = {"HD-2D", "Papel"};

const struct HdOptionInfo gHdOptionInfo[HD_OPT_COUNT] = {
    [HD_OPT_FIELD3D]   = {"field3d", "Campo 3D", true},
    [HD_OPT_ZOOM]      = {"zoom", "Zoom", HD_ZOOM_NORMAL, HD_ZOOM_COUNT, sZoomNames},
    [HD_OPT_STYLE]     = {"style", "Estilo", HD_STYLE_HD2D, HD_STYLE_COUNT, sStyleNames},
    [HD_OPT_HDSPRITES] = {"hdsprites", "Sprites HD", true},
    [HD_OPT_LIGHTING]  = {"lighting", "Luz do sol", true},
    [HD_OPT_DAYNIGHT]  = {"daynight", "Dia e noite", true},
    [HD_OPT_SHADOWS]   = {"shadows", "Sombras", true},
    [HD_OPT_DOF]       = {"dof", "Desfoque", true},
    [HD_OPT_BLOOM]     = {"bloom", "Brilho", true},
    [HD_OPT_GRADING]   = {"grading", "Névoa e cor", true},
    [HD_OPT_GRASS]     = {"grass", "Grama em pé", true},
    [HD_OPT_PARTICLES] = {"particles", "Partículas", true},
    [HD_OPT_FOLLOWER]  = {"follower", "Seguidor", true},
    [HD_OPT_FPS]       = {"fps", "Mostrar FPS", false},
};

static uint8_t sValues[HD_OPT_COUNT];
static const char *sProblems[HD_OPT_COUNT];
static uint32_t sVersion = 1;
static char sPath[512];
static bool sDirty;

static int FindOption(const char *key, size_t len)
{
    int i;

    for (i = 0; i < HD_OPT_COUNT; i++)
    {
        if (strlen(gHdOptionInfo[i].key) == len && strncmp(gHdOptionInfo[i].key, key, len) == 0)
            return i;
    }
    return -1;
}

int HdSettings_ValueCount(int option)
{
    if (option < 0 || option >= HD_OPT_COUNT)
        return 0;
    return gHdOptionInfo[option].valueCount > 0 ? gHdOptionInfo[option].valueCount : 2;
}

// "key=0" / "key=1" (or a value number) pairs separated by commas, spaces or new lines
static void ApplyPairs(const char *text)
{
    const char *p = text;

    while (*p != '\0')
    {
        const char *key, *eq;
        int option;

        while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
            p++;
        if (*p == '#')
        {
            while (*p != '\0' && *p != '\n')
                p++;
            continue;
        }
        key = p;
        while (*p != '\0' && *p != '=' && *p != ',' && *p != '\n')
            p++;
        if (*p != '=')
            continue;
        eq = p++;
        option = FindOption(key, eq - key);
        if (option >= 0)
        {
            int value = *p >= '0' && *p <= '9' ? atoi(p) : (*p == 's' || *p == 'y' || *p == 't');
            if (value >= HdSettings_ValueCount(option))
                value = HdSettings_ValueCount(option) - 1;
            sValues[option] = value;
        }
        while (*p != '\0' && *p != ',' && *p != '\n')
            p++;
    }
}

static void EnvOff(const char *name, int option)
{
    const char *value = getenv(name);
    if (value != NULL && value[0] == '0')
        sValues[option] = false;
}

void HdSettings_Init(const char *path)
{
    const char *value;
    int i;

    for (i = 0; i < HD_OPT_COUNT; i++)
        sValues[i] = gHdOptionInfo[i].defaultValue;

    sPath[0] = '\0';
    if (path != NULL)
    {
        FILE *file;
        snprintf(sPath, sizeof(sPath), "%s", path);
        file = fopen(sPath, "rb");
        if (file != NULL)
        {
            char text[1024];
            size_t len = fread(text, 1, sizeof(text) - 1, file);
            text[len] = '\0';
            fclose(file);
            ApplyPairs(text);
        }
    }

    if ((value = getenv("PE_FX")) != NULL && value[0] == '0')
    {
        sValues[HD_OPT_LIGHTING] = sValues[HD_OPT_SHADOWS] = sValues[HD_OPT_DOF] = false;
        sValues[HD_OPT_BLOOM] = sValues[HD_OPT_GRADING] = sValues[HD_OPT_PARTICLES] = false;
    }
    EnvOff("PE_DOF", HD_OPT_DOF);
    EnvOff("PE_PARTICLES", HD_OPT_PARTICLES);
    EnvOff("PE_FOLLOWER", HD_OPT_FOLLOWER);
    EnvOff("PE_FIELD3D", HD_OPT_FIELD3D);
    EnvOff("PE_HDMON", HD_OPT_HDSPRITES);
    if ((value = getenv("PE_HD")) != NULL)
        ApplyPairs(value);
    sVersion++;
}

bool HdSettings_Get(int option)
{
    return option >= 0 && option < HD_OPT_COUNT && sValues[option] != 0;
}

int HdSettings_GetValue(int option)
{
    return option >= 0 && option < HD_OPT_COUNT ? sValues[option] : 0;
}

void HdSettings_SetValue(int option, int value)
{
    if (option < 0 || option >= HD_OPT_COUNT || value < 0 || value >= HdSettings_ValueCount(option)
     || sValues[option] == value)
        return;
    sValues[option] = value;
    sVersion++;
    sDirty = true;
}

void HdSettings_Set(int option, bool value)
{
    HdSettings_SetValue(option, value ? 1 : 0);
}

void HdSettings_Toggle(int option)
{
    HdSettings_SetValue(option, (HdSettings_GetValue(option) + 1) % HdSettings_ValueCount(option));
}

void HdSettings_Step(int option, int delta)
{
    int value = HdSettings_GetValue(option) + delta;
    int count = HdSettings_ValueCount(option);

    HdSettings_SetValue(option, value < 0 ? 0 : (value >= count ? count - 1 : value));
}

uint32_t HdSettings_Version(void)
{
    return sVersion;
}

void HdSettings_SetProblem(int option, const char *text)
{
    if (option < 0 || option >= HD_OPT_COUNT || sProblems[option] == text)
        return;
    sProblems[option] = text;
    sVersion++;
}

const char *HdSettings_GetProblem(int option)
{
    return option >= 0 && option < HD_OPT_COUNT ? sProblems[option] : NULL;
}

void HdSettings_Save(void)
{
    FILE *file;
    int i;

    if (!sDirty || sPath[0] == '\0')
        return;
    sDirty = false;
    file = fopen(sPath, "wb");
    if (file == NULL)
    {
        fprintf(stderr, "settings: cannot write %s\n", sPath);
        return;
    }
    fprintf(file, "# Efeitos HD-2D (1 = ligado; zoom: 0 longe, 1 normal, 2 perto; style: 0 HD-2D, 1 papel). O menu do jogo (Tab ou ZL) muda este arquivo.\n");
    for (i = 0; i < HD_OPT_COUNT; i++)
        fprintf(file, "%s=%d\n", gHdOptionInfo[i].key, sValues[i]);
    fclose(file);
}

#endif // PORTABLE
