#ifdef PORTABLE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/follower_gfx.h"

#define RECORD_SIZE (4 + FOLLOWER_GFX_FRAMES * FOLLOWER_GFX_FRAME_SIZE + 64)

static uint8_t *sData;
static int sCount;
static uint16_t sPalettes[2][16]; // the last palette asked for, aligned

bool FollowerGfx_Init(const char *path)
{
    FILE *file = fopen(path, "rb");
    uint8_t header[8];
    size_t size;

    free(sData);
    sData = NULL;
    sCount = 0;
    if (file == NULL)
    {
        printf("follower: no %s, the following Pokémon is off\n", path);
        return false;
    }
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "FOL1", 4) != 0
     || (header[6] | (header[7] << 8)) != FOLLOWER_GFX_FRAMES)
    {
        fclose(file);
        fprintf(stderr, "follower: %s is not a FOL1 file (run hd2d/tools/follower_convert.py)\n", path);
        return false;
    }
    sCount = header[4] | (header[5] << 8);
    size = (size_t)sCount * RECORD_SIZE;
    sData = malloc(size);
    if (sData == NULL || fread(sData, 1, size, file) != size)
    {
        fclose(file);
        free(sData);
        sData = NULL;
        sCount = 0;
        fprintf(stderr, "follower: %s is truncated\n", path);
        return false;
    }
    fclose(file);
    printf("follower: %d sprites from %s\n", sCount, path);
    return true;
}

const uint8_t *FollowerGfx_Get(int key, bool shiny, const uint16_t **palette)
{
    const uint8_t *record;
    int i;

    if (sData == NULL || key < 0 || key >= sCount)
        return NULL;
    record = sData + (size_t)key * RECORD_SIZE;
    if (record[0] == 0)
        return NULL;
    if (palette != NULL)
    {
        const uint8_t *p = record + 4 + FOLLOWER_GFX_FRAMES * FOLLOWER_GFX_FRAME_SIZE + (shiny ? 32 : 0);
        for (i = 0; i < 16; i++)
            sPalettes[shiny][i] = p[i * 2] | (p[i * 2 + 1] << 8);
        *palette = sPalettes[shiny];
    }
    return record + 4;
}

#endif // PORTABLE
