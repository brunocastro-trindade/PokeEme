#ifdef PORTABLE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "data.h"
#include "palette.h"
#include "pokemon.h"
#include "constants/species.h"
#include "platform/hd_settings.h"
#include "platform/hd_sprites.h"
#include "platform/ppu_layers.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "third_party/stb_image.h"

#define PIC_FRAME_SIZE    0x800 // one 64x64 4bpp frame
#define PIC_MAX_FRAMES    4     // Castform has a frame per form
#define VIEW_FRONT        0
#define VIEW_BACK         1

// HD sprites keep the GBA pixel size, but are scaled down to fit at most
// HD_MAX_WIDTH wide and HD_MAX_RISE pixels above the original 64x64 frame
// (front pictures sit right below the top of the screen)
#define HD_MAX_WIDTH      96.0f
#define HD_MAX_RISE       8.0f

struct PicHash
{
    uint64_t hash;
    uint16_t species;
    uint8_t view;
    uint8_t frame;
    uint8_t anchorX;      // horizontal center of the opaque pixels
    uint8_t anchorBottom; // one past the lowest opaque row
};

struct SpeciesPalettes
{
    uint16_t normal[PIC_MAX_FRAMES][16];
    uint16_t shiny[PIC_MAX_FRAMES][16];
    uint8_t count; // 16-color palettes (one per Castform form)
};

struct AnimCacheEntry
{
    char key[40];
    struct HdAnim *anim; // NULL if the files do not exist
};

static bool sEnabled;      // data found and indexed
static char sDataDir[512];
static struct PicHash *sHashes;
static int sHashCount;
static struct SpeciesPalettes *sPalettes; // indexed by species
static struct AnimCacheEntry *sCache;
static int sCacheCount, sCacheCapacity;

static uint64_t HashBytes(const uint8_t *data, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ull; // FNV-1a
    size_t i;

    for (i = 0; i < len; i++)
    {
        h ^= data[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static int ComparePicHash(const void *a, const void *b)
{
    uint64_t ha = ((const struct PicHash *)a)->hash;
    uint64_t hb = ((const struct PicHash *)b)->hash;
    return ha < hb ? -1 : ha > hb;
}

static const struct PicHash *FindPicHash(uint64_t hash)
{
    int lo = 0, hi = sHashCount - 1;

    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (sHashes[mid].hash == hash)
            return &sHashes[mid];
        if (sHashes[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static uint32_t LzSize(const void *src)
{
    return ((const uint32_t *)src)[0] >> 8;
}

// Opaque bounds of a 64x64 4bpp frame in 1D tile order
static void FindAnchor(const uint8_t *frame, uint8_t *anchorX, uint8_t *anchorBottom)
{
    int minX = 64, maxX = -1, maxY = -1, x, y;

    for (y = 0; y < 64; y++)
    {
        for (x = 0; x < 64; x++)
        {
            int tile = (y / 8) * 8 + x / 8;
            uint8_t byte = frame[tile * 32 + (y % 8) * 4 + (x % 8) / 2];
            uint8_t pixel = (x & 1) ? byte >> 4 : byte & 0xF;
            if (pixel != 0)
            {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (maxX < 0)
    {
        *anchorX = 32;
        *anchorBottom = 64;
        return;
    }
    *anchorX = (minX + maxX + 1) / 2;
    *anchorBottom = maxY + 1;
}

static void AddPicHashes(const struct CompressedSpriteSheet *sheet, uint16_t species, uint8_t view, int *capacity)
{
    static uint8_t buffer[PIC_FRAME_SIZE * PIC_MAX_FRAMES];
    uint32_t size, frames, f;

    if (sheet->data == NULL)
        return;
    size = LzSize(sheet->data);
    if (size > sizeof(buffer) || size < PIC_FRAME_SIZE)
        return;
    LZ77UnCompWram(sheet->data, buffer);
    frames = size / PIC_FRAME_SIZE;
    for (f = 0; f < frames; f++)
    {
        struct PicHash *entry;
        if (sHashCount == *capacity)
        {
            *capacity *= 2;
            sHashes = realloc(sHashes, *capacity * sizeof(*sHashes));
        }
        entry = &sHashes[sHashCount++];
        entry->hash = HashBytes(buffer + f * PIC_FRAME_SIZE, PIC_FRAME_SIZE);
        entry->species = species;
        entry->view = view;
        entry->frame = f;
        FindAnchor(buffer + f * PIC_FRAME_SIZE, &entry->anchorX, &entry->anchorBottom);
    }
}

static int LoadPalettes(const struct CompressedSpritePalette *pal, uint16_t out[][16])
{
    static uint16_t buffer[16 * PIC_MAX_FRAMES];
    uint32_t size;

    if (pal->data == NULL)
        return 0;
    size = LzSize(pal->data);
    if (size > sizeof(buffer))
        return 0;
    LZ77UnCompWram(pal->data, buffer);
    memcpy(out, buffer, size);
    return size / 32;
}

bool HdSprites_Init(const char *dataDir)
{
    char probe[600];
    FILE *file;
    int capacity = 2048;
    uint16_t species;

    snprintf(sDataDir, sizeof(sDataDir), "%s", dataDir);
    snprintf(probe, sizeof(probe), "%s/front/25.hdm", sDataDir);
    file = fopen(probe, "rb");
    if (file == NULL)
    {
        printf("hdmon: no sprites in %s (run hd2d/tools/hdmon_convert.py); HD sprites off\n", sDataDir);
        return false;
    }
    fclose(file);

    sHashes = malloc(capacity * sizeof(*sHashes));
    sPalettes = calloc(SPECIES_UNOWN_QMARK + 1, sizeof(*sPalettes));
    for (species = 1; species <= SPECIES_UNOWN_QMARK; species++)
    {
        if (species == SPECIES_EGG)
            continue;
        AddPicHashes(&gMonFrontPicTable[species], species, VIEW_FRONT, &capacity);
        AddPicHashes(&gMonBackPicTable[species], species, VIEW_BACK, &capacity);
        sPalettes[species].count = LoadPalettes(&gMonPaletteTable[species], sPalettes[species].normal);
        LoadPalettes(&gMonShinyPaletteTable[species], sPalettes[species].shiny);
    }
    qsort(sHashes, sHashCount, sizeof(*sHashes), ComparePicHash);
    printf("hdmon: %d pictures indexed, sprites from %s\n", sHashCount, sDataDir);
    sEnabled = true;
    return true;
}

bool HdSprites_Enabled(void)
{
    return sEnabled;
}

void HdSprites_SetActive(bool active)
{
    HdSettings_Set(HD_OPT_HDSPRITES, active);
}

bool HdSprites_IsActive(void)
{
    return sEnabled && HdSettings_Get(HD_OPT_HDSPRITES);
}

// PokeAPI name of the animation for a picture, e.g. "25", "201-b", "351-sunny"
static bool AnimName(uint16_t species, uint8_t frame, char *out, size_t size)
{
    static const char *const sUnownForms[] = {
        "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p",
        "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "exclamation", "question",
    };
    static const char *const sCastformForms[] = {"351", "351-sunny", "351-rainy", "351-snowy"};
    uint16_t dex;

    if (species >= SPECIES_UNOWN_B && species <= SPECIES_UNOWN_QMARK)
    {
        snprintf(out, size, "201-%s", sUnownForms[species - SPECIES_UNOWN_B]);
        return true;
    }
    if (species == SPECIES_CASTFORM)
    {
        snprintf(out, size, "%s", sCastformForms[frame & 3]);
        return true;
    }
    if (species == SPECIES_DEOXYS)
    {
        // Emerald copies the Speed Forme frame over the Normal one when loading
        snprintf(out, size, frame == 0 ? "386-normal" : "386-speed");
        return true;
    }
    dex = SpeciesToNationalPokedexNum(species);
    if (dex == 0 || dex > 386)
        return false;
    snprintf(out, size, "%u", dex);
    return true;
}

static uint8_t *ReadFile(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *data;
    long len;

    if (file == NULL)
        return NULL;
    fseek(file, 0, SEEK_END);
    len = ftell(file);
    fseek(file, 0, SEEK_SET);
    data = malloc(len > 0 ? len : 1);
    if (data == NULL || fread(data, 1, len, file) != (size_t)len)
    {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = len;
    return data;
}

static uint16_t Read16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

static uint32_t Read32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static struct HdAnim *LoadAnim(const char *view, const char *name)
{
    char path[700];
    size_t metaSize, pngSize, need;
    uint8_t *meta, *png;
    struct HdAnim *anim;
    const uint8_t *p;
    int i, channels;
    uint16_t flags;

    snprintf(path, sizeof(path), "%s/%s/%s.hdm", sDataDir, view, name);
    meta = ReadFile(path, &metaSize);
    if (meta == NULL)
        return NULL;
    if (metaSize < 20 || memcmp(meta, "HDM1", 4) != 0)
    {
        fprintf(stderr, "hdmon: %s is not an HDM1 file\n", path);
        free(meta);
        return NULL;
    }

    anim = calloc(1, sizeof(*anim));
    p = meta + 4;
    anim->width = Read16(p);
    anim->height = Read16(p + 2);
    anim->columns = Read16(p + 4);
    anim->uniqueFrames = Read16(p + 6);
    anim->sequenceLength = Read16(p + 8);
    anim->paletteSize = Read16(p + 10);
    flags = Read16(p + 12);
    anim->hasShinyPalette = (flags & 1) != 0;
    p = meta + 20;

    need = 20 + anim->paletteSize * 4 * (anim->hasShinyPalette ? 2 : 1) + anim->sequenceLength * 4;
    if (anim->paletteSize > 256 || metaSize != need || anim->sequenceLength == 0)
    {
        fprintf(stderr, "hdmon: %s has an unexpected size\n", path);
        free(meta);
        free(anim);
        return NULL;
    }
    for (i = 0; i < anim->paletteSize; i++, p += 4)
        anim->palette[i] = Read32(p);
    if (anim->hasShinyPalette)
    {
        for (i = 0; i < anim->paletteSize; i++, p += 4)
            anim->shinyPalette[i] = Read32(p);
    }
    anim->frameIndex = malloc(anim->sequenceLength * sizeof(uint16_t));
    anim->durationMs = malloc(anim->sequenceLength * sizeof(uint16_t));
    for (i = 0; i < anim->sequenceLength; i++, p += 2)
        anim->frameIndex[i] = Read16(p);
    for (i = 0; i < anim->sequenceLength; i++, p += 2)
    {
        anim->durationMs[i] = Read16(p);
        anim->totalDurationMs += anim->durationMs[i];
    }
    free(meta);

    snprintf(path, sizeof(path), "%s/%s/%s.png", sDataDir, view, name);
    png = ReadFile(path, &pngSize);
    if (png != NULL)
        anim->atlas = stbi_load_from_memory(png, (int)pngSize, &anim->atlasWidth, &anim->atlasHeight, &channels, 1);
    free(png);
    if (anim->atlas == NULL)
    {
        fprintf(stderr, "hdmon: cannot read %s\n", path);
        free(anim->frameIndex);
        free(anim->durationMs);
        free(anim);
        return NULL;
    }
    return anim;
}

// Loads an animation once; later calls return the cached result (or NULL)
static struct HdAnim *GetAnim(const char *view, const char *name)
{
    char key[40];
    int i;

    snprintf(key, sizeof(key), "%s/%s", view, name);
    for (i = 0; i < sCacheCount; i++)
    {
        if (strcmp(sCache[i].key, key) == 0)
            return sCache[i].anim;
    }
    if (sCacheCount == sCacheCapacity)
    {
        sCacheCapacity = sCacheCapacity ? sCacheCapacity * 2 : 64;
        sCache = realloc(sCache, sCacheCapacity * sizeof(*sCache));
    }
    snprintf(sCache[sCacheCount].key, sizeof(sCache[sCacheCount].key), "%s", key);
    sCache[sCacheCount].anim = LoadAnim(view, name);
    if (sCache[sCacheCount].anim != NULL && !sCache[sCacheCount].anim->hasShinyPalette)
    {
        char shinyView[16];
        snprintf(shinyView, sizeof(shinyView), "%s-shiny", view);
        sCache[sCacheCount].anim->shinyAnim = LoadAnim(shinyView, name);
    }
    return sCache[sCacheCount++].anim;
}

static bool SameColors(const uint16_t *a, const uint16_t *b)
{
    int i;

    for (i = 1; i < 16; i++) // color 0 is transparent
    {
        if ((a[i] & 0x7FFF) != (b[i] & 0x7FFF))
            return false;
    }
    return true;
}

// Is the OBJ palette in use the species' shiny one? Unknown palettes count as normal.
static bool IsShinyPalette(uint16_t species, const uint16_t *colors)
{
    const struct SpeciesPalettes *pals = &sPalettes[species];
    int i;

    for (i = 0; i < pals->count; i++)
    {
        if (SameColors(colors, pals->normal[i]))
            return false;
        if (SameColors(colors, pals->shiny[i]))
            return true;
    }
    return false;
}

// The game fades palettes with BlendPalette():
//   faded = c + (((blend - c) * coeff) >> 4)   per 5-bit channel
// Recovers coeff and blend from the unfaded and displayed colors so the same
// fade can be applied to the HD sprite. Returns false if the palette was
// changed some other way.
static bool EstimatePaletteFade(const uint16_t *unfaded, const uint16_t *faded, float tint[4])
{
    int coeff, channel, blend[3], i;
    bool changed = false;

    for (i = 1; i < 16; i++)
    {
        if ((unfaded[i] & 0x7FFF) != (faded[i] & 0x7FFF))
            changed = true;
    }
    if (!changed)
    {
        tint[0] = tint[1] = tint[2] = tint[3] = 0.0f;
        return true;
    }

    for (coeff = 1; coeff <= 16; coeff++)
    {
        for (channel = 0; channel < 3; channel++)
        {
            int candidate;
            blend[channel] = -1;
            for (candidate = 0; candidate < 32 && blend[channel] < 0; candidate++)
            {
                bool fits = true;
                for (i = 1; i < 16 && fits; i++)
                {
                    int c = (unfaded[i] >> (channel * 5)) & 31;
                    int f = (faded[i] >> (channel * 5)) & 31;
                    if (c + (((candidate - c) * coeff) >> 4) != f)
                        fits = false;
                }
                if (fits)
                    blend[channel] = candidate;
            }
            if (blend[channel] < 0)
                break;
        }
        if (channel == 3)
        {
            tint[0] = blend[0] / 31.0f;
            tint[1] = blend[1] / 31.0f;
            tint[2] = blend[2] / 31.0f;
            tint[3] = coeff / 16.0f;
            return true;
        }
    }
    return false;
}

int HdSprites_Collect(struct HdSpriteDraw *draws, int maxDraws, uint8_t *skipOam, uint32_t timeMs)
{
    const struct OamData *oams = (const struct OamData *)OAM;
    int count = 0, i;

    memset(skipOam, 0, PPU_OAM_COUNT);
    if (!sEnabled || !HdSettings_Get(HD_OPT_HDSPRITES))
        return 0;

    for (i = 0; i < PPU_OAM_COUNT && count < maxDraws; i++)
    {
        const struct OamData *oam = &oams[i];
        const struct PicHash *pic;
        struct HdSpriteDraw *draw = &draws[count];
        struct HdAnim *anim;
        char name[24];
        const uint16_t *unfaded, *faded;
        bool affine, doubleSize, shiny;
        float half, cx, cy, m[2][2], inv[2][2], det, scale, ax, ay;
        int x, y, step, corner;

        // Pokémon pictures are 64x64 4bpp square sprites in 1D tile mapping
        if (oam->shape != 0 || oam->size != 3 || oam->bpp != 0 || oam->objMode == 2)
            continue;
        if (oam->affineMode == 2 || !(REG_DISPCNT & DISPCNT_OBJ_1D_MAP) || !(REG_DISPCNT & DISPCNT_OBJ_ON))
            continue;
        if (oam->tileNum + 64 > 1024)
            continue;
        pic = FindPicHash(HashBytes((const uint8_t *)OBJ_VRAM0 + oam->tileNum * 32, PIC_FRAME_SIZE));
        if (pic == NULL || !AnimName(pic->species, pic->frame, name, sizeof(name)))
            continue;
        anim = GetAnim(pic->view == VIEW_FRONT ? "front" : "back", name);
        if (anim == NULL)
            continue;

        unfaded = &gPlttBufferUnfaded[0x100 + oam->paletteNum * 16];
        faded = (const uint16_t *)(PLTT + 0x200) + oam->paletteNum * 16;
        if (!EstimatePaletteFade(unfaded, faded, draw->tint))
            continue; // some other palette effect: keep the original sprite
        shiny = IsShinyPalette(pic->species, unfaded);
        if (shiny && !anim->hasShinyPalette && anim->shinyAnim != NULL)
        {
            anim = anim->shinyAnim;
            shiny = false;
        }

        // Animation frame
        step = 0;
        if (anim->totalDurationMs > 0)
        {
            uint32_t local = timeMs % anim->totalDurationMs, acc = 0;
            for (step = 0; step < anim->sequenceLength - 1; step++)
            {
                acc += anim->durationMs[step];
                if (local < acc)
                    break;
            }
        }

        // Screen placement. The GBA maps screen to texture as
        //   tex = M * (screen - center) + 32
        // so a texture point lands at screen = center + M^-1 * (tex - 32).
        x = oam->x;
        y = oam->y;
        if (x >= DISPLAY_WIDTH)
            x -= 512;
        if (y >= DISPLAY_HEIGHT)
            y -= 256;
        affine = (oam->affineMode & 1) != 0;
        doubleSize = affine && (oam->affineMode & 2);
        half = doubleSize ? 64.0f : 32.0f;
        cx = x + half;
        cy = y + half;
        if (affine)
        {
            const struct OamData *base = &oams[oam->matrixNum * 4];
            m[0][0] = (int16_t)base[0].affineParam / 256.0f;
            m[0][1] = (int16_t)base[1].affineParam / 256.0f;
            m[1][0] = (int16_t)base[2].affineParam / 256.0f;
            m[1][1] = (int16_t)base[3].affineParam / 256.0f;
        }
        else
        {
            m[0][0] = (oam->matrixNum & ST_OAM_HFLIP) ? -1.0f : 1.0f;
            m[1][1] = (oam->matrixNum & ST_OAM_VFLIP) ? -1.0f : 1.0f;
            m[0][1] = m[1][0] = 0.0f;
        }
        det = m[0][0] * m[1][1] - m[0][1] * m[1][0];
        if (det > -1e-6f && det < 1e-6f)
            continue; // scaled to nothing
        inv[0][0] = m[1][1] / det;
        inv[0][1] = -m[0][1] / det;
        inv[1][0] = -m[1][0] / det;
        inv[1][1] = m[0][0] / det;

        // The HD frame stands on the same point as the original picture
        scale = 1.0f;
        if (anim->width * scale > HD_MAX_WIDTH)
            scale = HD_MAX_WIDTH / anim->width;
        if (anim->height * scale > pic->anchorBottom + HD_MAX_RISE)
            scale = (pic->anchorBottom + HD_MAX_RISE) / anim->height;
        ax = pic->anchorX - anim->width * scale / 2.0f;
        ay = pic->anchorBottom - anim->height * scale;
        for (corner = 0; corner < 4; corner++)
        {
            float tx = ax + ((corner & 1) ? anim->width : 0) * scale - 32.0f;
            float ty = ay + ((corner & 2) ? anim->height : 0) * scale - 32.0f;
            draw->corners[corner][0] = cx + inv[0][0] * tx + inv[0][1] * ty;
            draw->corners[corner][1] = cy + inv[1][0] * tx + inv[1][1] * ty;
        }

        draw->anim = anim;
        draw->atlasFrame = anim->frameIndex[step];
        draw->shiny = shiny;
        draw->oamIndex = i;
        draw->priority = oam->priority;
        draw->semiTransparent = oam->objMode == 1;
        draw->mosaic = oam->mosaic;
        skipOam[i] = 1;
        count++;
    }
    return count;
}

#endif // PORTABLE
