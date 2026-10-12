#ifdef PORTABLE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "platform/harness.h"

// Input script format, one event per line ('#' starts a comment):
//
//   <frame> <keys> [duration]
//
// <keys> is '-' (nothing) or button names joined by '+':
// A B SELECT START RIGHT LEFT UP DOWN R L. The keys stay held from <frame>
// until the next event; with [duration] they are released after that many
// frames. Example:
//
//   300 START 2      # press Start for 2 frames at frame 300
//   400 A+UP         # hold A and Up from frame 400 on

#define MAX_INPUT_EVENTS 4096
#define MAX_DUMP_AT      256

struct InputEvent
{
    uint32_t frame;
    uint16_t keys;
};

static bool sHeadless;
enum { RENDERER_DEFAULT, RENDERER_GL, RENDERER_SDL };
static int sRenderer = RENDERER_DEFAULT;
static bool sVerifyGl;
static int sComposeScale = 1;
static bool sHdSprites = true;
static bool sField3D = true;
static const char *sDataDir;
static uint32_t sMismatchFrames;
static uint32_t sMaxFrames;
static uint32_t sFrame;
#ifdef __SWITCH__
static const char *sSavePath = "sdmc:/switch/pokeemerald/pokeemerald.sav";
#else
static const char *sSavePath = "pokeemerald.sav";
#endif
static const char *sDumpDir = ".";
static uint32_t sDumpEvery;
static uint32_t sDumpAt[MAX_DUMP_AT];
static int sDumpAtCount;

static struct InputEvent sEvents[MAX_INPUT_EVENTS];
static int sEventCount;
static int sNextEvent;
static bool sHasScript;
static uint16_t sScriptKeys;

static uint16_t ParseKeys(char *text)
{
    static const struct { const char *name; uint16_t key; } sNames[] = {
        {"A", A_BUTTON}, {"B", B_BUTTON}, {"SELECT", SELECT_BUTTON}, {"START", START_BUTTON},
        {"RIGHT", DPAD_RIGHT}, {"LEFT", DPAD_LEFT}, {"UP", DPAD_UP}, {"DOWN", DPAD_DOWN},
        {"R", R_BUTTON}, {"L", L_BUTTON},
    };
    uint16_t keys = 0;
    char *save, *tok;

    if (strcmp(text, "-") == 0)
        return 0;
    for (tok = strtok_r(text, "+", &save); tok != NULL; tok = strtok_r(NULL, "+", &save))
    {
        size_t i;
        for (i = 0; i < sizeof(sNames) / sizeof(sNames[0]); i++)
        {
            if (strcmp(tok, sNames[i].name) == 0)
            {
                keys |= sNames[i].key;
                break;
            }
        }
        if (i == sizeof(sNames) / sizeof(sNames[0]))
            fprintf(stderr, "harness: unknown key '%s'\n", tok);
    }
    return keys;
}

static void AddEvent(uint32_t frame, uint16_t keys)
{
    int i;

    if (sEventCount == MAX_INPUT_EVENTS)
    {
        fprintf(stderr, "harness: too many input events\n");
        return;
    }
    // Keep the list sorted by frame; events on the same frame keep file order
    for (i = sEventCount; i > 0 && sEvents[i - 1].frame > frame; i--)
        sEvents[i] = sEvents[i - 1];
    sEvents[i].frame = frame;
    sEvents[i].keys = keys;
    sEventCount++;
}

static void LoadInputScript(const char *path)
{
    char line[256];
    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        fprintf(stderr, "harness: cannot open input script %s\n", path);
        exit(1);
    }
    while (fgets(line, sizeof(line), file) != NULL)
    {
        char keyText[128];
        unsigned frame, duration;
        char *comment = strchr(line, '#');
        int fields;

        if (comment != NULL)
            *comment = '\0';
        fields = sscanf(line, "%u %127s %u", &frame, keyText, &duration);
        if (fields < 2)
            continue;
        AddEvent(frame, ParseKeys(keyText));
        if (fields == 3)
            AddEvent(frame + duration, 0);
    }
    fclose(file);
    sHasScript = true;
}

static void ParseDumpAt(const char *list)
{
    const char *p = list;

    while (*p != '\0' && sDumpAtCount < MAX_DUMP_AT)
    {
        char *end;
        unsigned long value = strtoul(p, &end, 10);
        if (end == p)
            break;
        sDumpAt[sDumpAtCount++] = (uint32_t)value;
        p = (*end == ',') ? end + 1 : end;
    }
}

void Harness_Init(void)
{
    const char *value;

    if ((value = getenv("PE_HEADLESS")) != NULL && value[0] == '1')
    {
        sHeadless = true;
        setvbuf(stdout, NULL, _IOLBF, 0); // keep the log if the game crashes
    }
    if ((value = getenv("PE_FRAMES")) != NULL)
        sMaxFrames = (uint32_t)strtoul(value, NULL, 10);
    if ((value = getenv("PE_SAVE")) != NULL)
        sSavePath = value;
    if ((value = getenv("PE_DUMP_DIR")) != NULL)
        sDumpDir = value;
    if ((value = getenv("PE_DUMP_EVERY")) != NULL)
        sDumpEvery = (uint32_t)strtoul(value, NULL, 10);
    if ((value = getenv("PE_DUMP_AT")) != NULL)
        ParseDumpAt(value);
    if ((value = getenv("PE_INPUT")) != NULL)
        LoadInputScript(value);
    if ((value = getenv("PE_RENDERER")) != NULL)
        sRenderer = strcmp(value, "sdl") == 0 ? RENDERER_SDL : RENDERER_GL;
    if ((value = getenv("PE_VERIFY_GL")) != NULL && value[0] == '1')
        sVerifyGl = true;
    if ((value = getenv("PE_SCALE")) != NULL)
        sComposeScale = atoi(value) > 0 ? atoi(value) : 1;
    if ((value = getenv("PE_HDMON")) != NULL && value[0] == '0')
        sHdSprites = false;
    if ((value = getenv("PE_FIELD3D")) != NULL && value[0] == '0')
        sField3D = false;
    sDataDir = getenv("PE_DATA_DIR");
}

int Harness_ComposeScale(void)
{
    return sComposeScale;
}

bool Harness_HdSpritesEnabled(void)
{
    return sHdSprites && !sVerifyGl;
}

bool Harness_Field3DEnabled(void)
{
    return sField3D && !sVerifyGl;
}

const char *Harness_DataDir(void)
{
    return sDataDir;
}

const char *Harness_DumpDir(void)
{
    return sDumpDir;
}

void Harness_DumpRgb(const uint8_t *rgb, int width, int height)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/frame_%06u.png", sDumpDir, sFrame);
    if (!Harness_WritePngRgb(path, rgb, width, height))
        fprintf(stderr, "harness: failed to write %s\n", path);
}

// Windowed runs use the GL compositor unless told otherwise; headless runs
// only when asked to (PE_RENDERER=gl or PE_VERIFY_GL=1).
bool Harness_UseGl(void)
{
    if (sVerifyGl || sRenderer == RENDERER_GL)
        return true;
    return sRenderer == RENDERER_DEFAULT && !sHeadless;
}

bool Harness_VerifyGl(void)
{
    return sVerifyGl;
}

void Harness_ReportMismatch(const uint16_t *software, const uint16_t *gpu)
{
    int i, count = 0, first = -1;

    for (i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
    {
        if ((software[i] & 0x7FFF) != (gpu[i] & 0x7FFF))
        {
            if (first < 0)
                first = i;
            count++;
        }
    }
    if (count == 0)
        return;
    if (sMismatchFrames < 10)
    {
        char path[1024];
        fprintf(stderr, "verify: frame %u differs in %d pixels, first at (%d,%d): software %04X gpu %04X\n",
                sFrame, count, first % DISPLAY_WIDTH, first / DISPLAY_WIDTH,
                software[first] & 0x7FFF, gpu[first] & 0x7FFF);
        snprintf(path, sizeof(path), "%s/mismatch_%06u_sw.png", sDumpDir, sFrame);
        Harness_WritePng(path, software, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        snprintf(path, sizeof(path), "%s/mismatch_%06u_gpu.png", sDumpDir, sFrame);
        Harness_WritePng(path, gpu, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }
    sMismatchFrames++;
}

bool Harness_Finish(void)
{
    if (Harness_VerifyGl())
        printf("verify: %u frames checked, %u differ\n", sFrame, sMismatchFrames);
    return sMismatchFrames == 0;
}

bool Harness_IsHeadless(void)
{
    return sHeadless;
}

const char *Harness_SavePath(void)
{
    return sSavePath;
}

uint32_t Harness_FrameCount(void)
{
    return sFrame;
}

uint16_t Harness_GetKeys(uint16_t liveKeys)
{
    if (!sHasScript)
        return liveKeys;
    while (sNextEvent < sEventCount && sEvents[sNextEvent].frame <= sFrame)
        sScriptKeys = sEvents[sNextEvent++].keys;
    return sScriptKeys;
}

static bool ShouldDump(uint32_t frame);

bool Harness_WantsImage(void)
{
    return ShouldDump(sFrame);
}

static bool ShouldDump(uint32_t frame)
{
    int i;

    if (sDumpEvery != 0 && frame % sDumpEvery == 0)
        return true;
    for (i = 0; i < sDumpAtCount; i++)
    {
        if (sDumpAt[i] == frame)
            return true;
    }
    return false;
}

bool Harness_EndFrame(const uint16_t *image)
{
    if (image != NULL && ShouldDump(sFrame))
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s/frame_%06u.png", sDumpDir, sFrame);
        if (!Harness_WritePng(path, image, DISPLAY_WIDTH, DISPLAY_HEIGHT))
            fprintf(stderr, "harness: failed to write %s\n", path);
    }
    sFrame++;
    return sMaxFrames == 0 || sFrame < sMaxFrames;
}

// Minimal PNG writer (zlib "stored" blocks, no compression), so the
// harness needs no image library on any platform.

static uint32_t Crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    static uint32_t table[256];
    static bool tableReady;
    size_t i;

    if (!tableReady)
    {
        uint32_t n, k;
        for (n = 0; n < 256; n++)
        {
            uint32_t c = n;
            for (k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        tableReady = true;
    }
    crc = ~crc;
    for (i = 0; i < len; i++)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void PutBE32(uint8_t *dst, uint32_t value)
{
    dst[0] = value >> 24;
    dst[1] = value >> 16;
    dst[2] = value >> 8;
    dst[3] = value;
}

static bool WriteChunk(FILE *file, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t header[8], trailer[4];
    uint32_t crc;

    PutBE32(header, len);
    memcpy(header + 4, type, 4);
    crc = Crc32(0, header + 4, 4);
    crc = Crc32(crc, data, len);
    PutBE32(trailer, crc);
    return fwrite(header, 1, 8, file) == 8
        && fwrite(data, 1, len, file) == len
        && fwrite(trailer, 1, 4, file) == 4;
}

bool Harness_WritePng(const char *path, const uint16_t *pixels, int width, int height)
{
    uint8_t *rgb = malloc((size_t)width * height * 3);
    int i;
    bool ok;

    if (rgb == NULL)
        return false;
    for (i = 0; i < width * height; i++)
    {
        uint16_t c = pixels[i];
        uint8_t r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
        rgb[i * 3 + 0] = (r << 3) | (r >> 2);
        rgb[i * 3 + 1] = (g << 3) | (g >> 2);
        rgb[i * 3 + 2] = (b << 3) | (b >> 2);
    }
    ok = Harness_WritePngRgb(path, rgb, width, height);
    free(rgb);
    return ok;
}

bool Harness_WritePngRgb(const char *path, const uint8_t *rgb, int width, int height)
{
    static const uint8_t sSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    size_t rowLen = 1 + (size_t)width * 3;
    size_t rawLen = rowLen * height;
    size_t blocks = (rawLen + 65534) / 65535;
    size_t zlen = 2 + rawLen + blocks * 5 + 4;
    uint8_t *raw = malloc(rawLen);
    uint8_t *z = malloc(zlen);
    uint8_t ihdr[13];
    uint32_t a = 1, b = 0;
    size_t i, pos, src;
    FILE *file;
    bool ok;
    int y;

    if (raw == NULL || z == NULL)
    {
        free(raw);
        free(z);
        return false;
    }

    for (y = 0; y < height; y++)
    {
        uint8_t *row = raw + y * rowLen;
        row[0] = 0; // filter: none
        memcpy(row + 1, rgb + (size_t)y * width * 3, (size_t)width * 3);
    }

    pos = 0;
    z[pos++] = 0x78;
    z[pos++] = 0x01;
    for (src = 0; src < rawLen;)
    {
        size_t len = rawLen - src > 65535 ? 65535 : rawLen - src;
        z[pos++] = (src + len == rawLen) ? 1 : 0;
        z[pos++] = len & 0xFF;
        z[pos++] = len >> 8;
        z[pos++] = ~len & 0xFF;
        z[pos++] = (~len >> 8) & 0xFF;
        memcpy(z + pos, raw + src, len);
        pos += len;
        src += len;
    }
    for (i = 0; i < rawLen; i++)
    {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    PutBE32(z + pos, (b << 16) | a);
    pos += 4;

    PutBE32(ihdr, width);
    PutBE32(ihdr + 4, height);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 2;  // color type: RGB
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    file = fopen(path, "wb");
    ok = file != NULL
      && fwrite(sSignature, 1, 8, file) == 8
      && WriteChunk(file, "IHDR", ihdr, sizeof(ihdr))
      && WriteChunk(file, "IDAT", z, (uint32_t)pos)
      && WriteChunk(file, "IEND", NULL, 0);
    if (file != NULL)
        ok = (fclose(file) == 0) && ok;
    free(raw);
    free(z);
    return ok;
}

#endif // PORTABLE
