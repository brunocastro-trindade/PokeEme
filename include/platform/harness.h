#ifndef GUARD_PLATFORM_HARNESS_H
#define GUARD_PLATFORM_HARNESS_H

#include <stdbool.h>
#include <stdint.h>

// Test harness: lets the game run without a window, follow a scripted
// input file and dump frames as PNG. Everything is configured through
// environment variables, so normal runs are unaffected:
//
//   PE_HEADLESS=1          no window, no audio device, run unthrottled
//   PE_FRAMES=N            quit after N frames
//   PE_INPUT=file          scripted input (see harness.c for the format)
//   PE_DUMP_DIR=dir        where frame PNGs are written (default ".")
//   PE_DUMP_EVERY=K        dump every K-th frame
//   PE_DUMP_AT=a,b,c       dump these frames
//   PE_SAVE=file           save file path (default "pokeemerald.sav")
//   PE_RENDERER=gl|sdl     video output: OpenGL compositor or SDL_Renderer
//                          (default: gl with a window, software when headless)
//   PE_VERIFY_GL=1         compose every frame both on the GPU and in software,
//                          report any pixel that differs (turns HD sprites off)
//   PE_SCALE=N             compose at N x the GBA resolution (headless; default 1)
//   PE_HDMON=0             turn the HD Pokémon sprites off
//   PE_FIELD3D=0           turn the 3D field off
//   PE_FX=0                3D field without lighting, shadows, particles and post-processing
//   PE_DOF=0               3D field without the depth-of-field blur
//   PE_PARTICLES=0         no particle effects
//   PE_HD="key=0,key=1"    any HD option by name (hd_settings.c)
//   PE_TEST_MENU="f:keys"  drive the options menu from frame f (sdl2.c)
//   PE_DATA_DIR=dir        converted data (default: hd2d/data next to the executable)

void Harness_Init(void);
bool Harness_IsHeadless(void);
const char *Harness_SavePath(void);
uint32_t Harness_FrameCount(void);
bool Harness_UseGl(void);
bool Harness_VerifyGl(void);
int Harness_ComposeScale(void);
bool Harness_HdSpritesEnabled(void);
bool Harness_Field3DEnabled(void);
const char *Harness_DataDir(void); // NULL: platform default
const char *Harness_DumpDir(void);

// True when the current frame is going to be dumped.
bool Harness_WantsImage(void);

// Counts a frame whose GPU and software images differ (PE_VERIFY_GL).
void Harness_ReportMismatch(const uint16_t *software, const uint16_t *gpu);
// Prints the verification summary; returns false if any frame differed.
bool Harness_Finish(void);

// Returns the keys to feed the game this frame. While an input script is
// loaded its keys replace the live ones.
uint16_t Harness_GetKeys(uint16_t liveKeys);

// Called once per emulated frame with the finished 240x160 BGR555 image
// (NULL if the frame was not drawn). Returns false when the run must stop.
bool Harness_EndFrame(const uint16_t *image);

// Dumps an RGB8 image of any size as this frame's capture.
void Harness_DumpRgb(const uint8_t *rgb, int width, int height);

// Write a BGR555 / RGB8 image as PNG. Return false on I/O error.
bool Harness_WritePng(const char *path, const uint16_t *pixels, int width, int height);
bool Harness_WritePngRgb(const char *path, const uint8_t *rgb, int width, int height);

#endif // GUARD_PLATFORM_HARNESS_H
