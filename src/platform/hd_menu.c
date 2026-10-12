#ifdef PORTABLE
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "fonts.h"
#include "platform/hd_menu.h"
#include "platform/hd_settings.h"

// Layout, in GBA pixels
#define WIN_X0     4
#define WIN_X1     150
#define WIN_Y0     2
#define WIN_Y1     158
#define TITLE_Y    4
#define ROWS_Y     22
#define ROW_H      9
#define VALUE_X    (WIN_X1 - 50)
#define HINT_Y     (TITLE_Y + 2) // on the title line, at the right

#define REPEAT_DELAY 18
#define REPEAT_EVERY 5

struct Rgba
{
    uint8_t r, g, b, a;
};

static const struct Rgba sWindow = {248, 248, 248, 230};
static const struct Rgba sBorder = {80, 88, 112, 255};
static const struct Rgba sText = {72, 72, 80, 255};
static const struct Rgba sTextShadow = {208, 208, 216, 255};
static const struct Rgba sTitle = {48, 80, 152, 255};
static const struct Rgba sTitleShadow = {200, 212, 240, 255};
static const struct Rgba sCursorBar = {204, 226, 250, 255};
static const struct Rgba sOn = {40, 136, 64, 255};
static const struct Rgba sOnShadow = {176, 224, 184, 255};
static const struct Rgba sOff = {192, 64, 56, 255};
static const struct Rgba sOffShadow = {240, 196, 188, 255};
static const struct Rgba sProblem = {200, 112, 24, 255};
static const struct Rgba sProblemShadow = {244, 212, 176, 255};
static const struct Rgba sFpsBack = {0, 0, 0, 150};
static const struct Rgba sWhite = {255, 255, 255, 255};
static const struct Rgba sGray = {60, 60, 60, 255};

static uint8_t sImage[HDMENU_WIDTH * HDMENU_HEIGHT * 4];
static uint32_t sImageVersion;
static bool sOpen;
static bool sBlockUntilRelease;
static int sCursor;
static uint32_t sPrevHeld;
static int sRepeatTimer;
static uint32_t sDrawnSettings;
static bool sDrawnOpen;
static int sDrawnCursor = -1;
static int sDrawnFps = -1;

// --- text ----------------------------------------------------------------

struct Font
{
    const u16 *glyphs;
    const u8 *widths;
};

static const struct Font sNormal = {gFontNormalLatinGlyphs, gFontNormalLatinGlyphWidths};
static const struct Font sSmall = {gFontSmallLatinGlyphs, gFontSmallLatinGlyphWidths};

// Unicode code point -> glyph in the game's charset (charmap.txt); -1: none.
// *tilde: draw a tilde over it (the charset has no ã or õ).
static int GlyphFor(uint32_t cp, bool *tilde)
{
    static const struct { uint16_t cp; uint8_t code; } sAccents[] = {
        {0xC0, 0x01}, {0xC1, 0x02}, {0xC2, 0x03}, {0xC7, 0x04}, {0xC9, 0x06}, {0xCA, 0x07}, {0xCD, 0x5A},
        {0xD3, 0x0E}, {0xD4, 0x0F}, {0xDA, 0x12}, {0xE0, 0x16}, {0xE1, 0x17}, {0xE2, 0x68}, {0xE7, 0x19},
        {0xE9, 0x1B}, {0xEA, 0x1C}, {0xED, 0x6F}, {0xF3, 0x23}, {0xF4, 0x24}, {0xFA, 0x27}, {0x25B6, 0xEF},
    };
    size_t i;

    *tilde = false;
    if (cp == ' ')
        return 0x00;
    if (cp >= 'A' && cp <= 'Z')
        return 0xBB + (cp - 'A');
    if (cp >= 'a' && cp <= 'z')
        return 0xD5 + (cp - 'a');
    if (cp >= '0' && cp <= '9')
        return 0xA1 + (cp - '0');
    switch (cp)
    {
    case '!': return 0xAB;
    case '?': return 0xAC;
    case '.': return 0xAD;
    case '-': return 0xAE;
    case ',': return 0xB8;
    case '/': return 0xBA;
    case ':': return 0xF0;
    case '(': return 0x5C;
    case ')': return 0x5D;
    case '%': return 0x5B;
    case '+': return 0x2E;
    case '&': return 0x2D;
    case '=': return 0x35;
    case 0xE3: *tilde = true; return 0xD5;      // ã
    case 0xF5: *tilde = true; return 0xD5 + 14; // õ
    }
    for (i = 0; i < sizeof(sAccents) / sizeof(sAccents[0]); i++)
    {
        if (sAccents[i].cp == cp)
            return sAccents[i].code;
    }
    return -1;
}

static uint32_t NextCodePoint(const char **text)
{
    const uint8_t *s = (const uint8_t *)*text;
    uint32_t cp;

    if (s[0] < 0x80)
    {
        cp = s[0];
        *text += 1;
    }
    else if ((s[0] & 0xE0) == 0xC0 && s[1] != 0)
    {
        cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *text += 2;
    }
    else if ((s[0] & 0xF0) == 0xE0 && s[1] != 0 && s[2] != 0)
    {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *text += 3;
    }
    else
    {
        cp = '?';
        *text += 1;
    }
    return cp;
}

static void Put(int x, int y, struct Rgba c)
{
    uint8_t *p;

    if (x < 0 || y < 0 || x >= HDMENU_WIDTH || y >= HDMENU_HEIGHT)
        return;
    p = &sImage[(y * HDMENU_WIDTH + x) * 4];
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
    p[3] = c.a;
}

static void FillRect(int x0, int y0, int x1, int y1, struct Rgba c)
{
    int x, y;

    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            Put(x, y, c);
}

// Draws one glyph (pixel value 1 = ink, 2 = shadow); returns its width
static int DrawGlyph(const struct Font *font, int code, bool tilde, int x, int y, struct Rgba ink, struct Rgba shadow)
{
    const u16 *glyph = font->glyphs + 0x20 * code;
    int width = font->widths[code], tile, row, col;

    for (tile = 0; tile < 4; tile++)
    {
        for (row = 0; row < 8; row++)
        {
            u16 bits = glyph[tile * 8 + row];
            for (col = 0; col < 8; col++)
            {
                int v = (bits >> (14 - 2 * col)) & 3;
                int px = (tile & 1) * 8 + col, py = (tile >> 1) * 8 + row;
                if (px >= width)
                    continue;
                if (v == 1)
                    Put(x + px, y + py, ink);
                else if (v == 2)
                    Put(x + px, y + py, shadow);
            }
        }
    }
    if (tilde)
    {
        static const char *sTilde[2] = {".##.#", "#.##."};
        int top = font == &sNormal ? 3 : 3;
        for (row = 0; row < 2; row++)
            for (col = 0; col < 5; col++)
                if (sTilde[row][col] == '#')
                {
                    Put(x + col, y + top + row, ink);
                    Put(x + col + 1, y + top + row, shadow);
                }
    }
    return width;
}

static int TextWidth(const struct Font *font, const char *text)
{
    int width = 0;

    while (*text != '\0')
    {
        bool tilde;
        int code = GlyphFor(NextCodePoint(&text), &tilde);
        if (code >= 0)
            width += font->widths[code];
    }
    return width;
}

static int DrawText(const struct Font *font, const char *text, int x, int y, struct Rgba ink, struct Rgba shadow)
{
    while (*text != '\0')
    {
        bool tilde;
        int code = GlyphFor(NextCodePoint(&text), &tilde);
        if (code >= 0)
            x += DrawGlyph(font, code, tilde, x, y, ink, shadow);
    }
    return x;
}

// --- drawing -------------------------------------------------------------

static void DrawMenu(void)
{
    int i;

    FillRect(WIN_X0 + 1, WIN_Y0 + 1, WIN_X1 - 1, WIN_Y1 - 1, sWindow);
    FillRect(WIN_X0 + 1, WIN_Y0, WIN_X1 - 1, WIN_Y0 + 1, sBorder);
    FillRect(WIN_X0 + 1, WIN_Y1 - 1, WIN_X1 - 1, WIN_Y1, sBorder);
    FillRect(WIN_X0, WIN_Y0 + 1, WIN_X0 + 1, WIN_Y1 - 1, sBorder);
    FillRect(WIN_X1 - 1, WIN_Y0 + 1, WIN_X1, WIN_Y1 - 1, sBorder);

    DrawText(&sNormal, "Efeitos HD-2D", WIN_X0 + 8, TITLE_Y, sTitle, sTitleShadow);
    FillRect(WIN_X0 + 6, ROWS_Y - 3, WIN_X1 - 6, ROWS_Y - 2, sTextShadow);

    for (i = 0; i < HD_OPT_COUNT; i++)
    {
        int y = ROWS_Y + i * ROW_H;
        bool on = HdSettings_Get(i);
        const char *value = on ? "Sim" : "Não";

        if (gHdOptionInfo[i].valueNames != NULL)
        {
            value = gHdOptionInfo[i].valueNames[HdSettings_GetValue(i)];
            on = true; // a choice, not on/off: always in the "on" color
        }

        if (i == sCursor)
        {
            FillRect(WIN_X0 + 4, y + 2, WIN_X1 - 4, y + ROW_H + 2, sCursorBar);
            DrawText(&sSmall, "▶", WIN_X0 + 6, y - 1, sText, sTextShadow);
        }
        DrawText(&sSmall, gHdOptionInfo[i].label, WIN_X0 + 14, y - 1, sText, sTextShadow);
        if (HdSettings_GetProblem(i) != NULL && on)
            DrawText(&sSmall, HdSettings_GetProblem(i), VALUE_X, y - 1, sProblem, sProblemShadow);
        else
            DrawText(&sSmall, value, VALUE_X, y - 1, on ? sOn : sOff, on ? sOnShadow : sOffShadow);
    }
    {
        const char *hint = "A muda  B sai";
        DrawText(&sSmall, hint, WIN_X1 - 6 - TextWidth(&sSmall, hint), HINT_Y, sText, sTextShadow);
    }
}

static void DrawFps(int fps)
{
    char text[16];
    int width;

    snprintf(text, sizeof(text), "%d FPS", fps);
    width = TextWidth(&sSmall, text);
    FillRect(1, 1, width + 7, 13, sFpsBack);
    DrawText(&sSmall, text, 4, -2, sWhite, sGray);
}

// --- input ---------------------------------------------------------------

void HdMenu_Open(void)
{
    sOpen = true;
    sBlockUntilRelease = true;
}

static void Close(void)
{
    sOpen = false;
    sBlockUntilRelease = true;
    HdSettings_Save();
}

void HdMenu_Update(uint32_t held, float fps)
{
    uint32_t pressed = held & ~sPrevHeld;
    int fpsShown = HdSettings_Get(HD_OPT_FPS) ? (int)(fps + 0.5f) : -1;

    // up/down repeat while held
    if (held & (HDMENU_UP | HDMENU_DOWN))
    {
        if (pressed & (HDMENU_UP | HDMENU_DOWN))
            sRepeatTimer = REPEAT_DELAY;
        else if (--sRepeatTimer <= 0)
        {
            pressed |= held & (HDMENU_UP | HDMENU_DOWN);
            sRepeatTimer = REPEAT_EVERY;
        }
    }
    sPrevHeld = held;

    if (!sOpen)
    {
        if (pressed & HDMENU_OPEN)
            HdMenu_Open();
    }
    else if (pressed & (HDMENU_OPEN | HDMENU_BACK))
    {
        Close();
    }
    else
    {
        if (pressed & HDMENU_UP)
            sCursor = (sCursor + HD_OPT_COUNT - 1) % HD_OPT_COUNT;
        if (pressed & HDMENU_DOWN)
            sCursor = (sCursor + 1) % HD_OPT_COUNT;
        if (pressed & HDMENU_CONFIRM)
            HdSettings_Toggle(sCursor);
        if (pressed & HDMENU_LEFT)
            HdSettings_Step(sCursor, -1);
        if (pressed & HDMENU_RIGHT)
            HdSettings_Step(sCursor, 1);
        fpsShown = HdSettings_Get(HD_OPT_FPS) ? (int)(fps + 0.5f) : -1;
    }
    if (!sOpen && sBlockUntilRelease && held == 0)
        sBlockUntilRelease = false;

    if (sOpen != sDrawnOpen || sCursor != sDrawnCursor || HdSettings_Version() != sDrawnSettings || fpsShown != sDrawnFps)
    {
        memset(sImage, 0, sizeof(sImage));
        if (fpsShown >= 0)
            DrawFps(fpsShown);
        if (sOpen)
            DrawMenu();
        sDrawnOpen = sOpen;
        sDrawnCursor = sCursor;
        sDrawnSettings = HdSettings_Version();
        sDrawnFps = fpsShown;
        sImageVersion++;
    }
}

bool HdMenu_IsOpen(void)
{
    return sOpen;
}

bool HdMenu_BlocksGameInput(void)
{
    return sOpen || sBlockUntilRelease;
}

const uint8_t *HdMenu_Overlay(uint32_t *version)
{
    *version = sImageVersion;
    if (!sDrawnOpen && sDrawnFps < 0)
        return NULL;
    return sImage;
}

#endif // PORTABLE
