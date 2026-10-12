#ifndef GUARD_PLATFORM_H
#define GUARD_PLATFORM_H

#include "global.h"
#include "siirtc.h"

void Platform_StoreSaveFile(void);
void Platform_ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size);
void Platform_QueueAudio(float *audioBuffer, s32 samplesPerFrame);
u16 Platform_GetKeyInput(void);
void Platform_GetStatus(struct SiiRtcInfo *rtc);
void Platform_SetStatus(struct SiiRtcInfo *rtc);
static void UpdateInternalClock(void);
void Platform_GetDateTime(struct SiiRtcInfo *rtc);
void Platform_SetDateTime(struct SiiRtcInfo *rtc);
void Platform_GetTime(struct SiiRtcInfo *rtc);
void Platform_SetTime(struct SiiRtcInfo *rtc);
void Platform_SetAlarm(u8 *alarmData);

// Called right after the OAM buffer is copied to OAM (VBlank)
void Platform_OnLoadOam(void);

// A door animation drew these 8 tile entries over the map cell (x, y)
// (gBackupMapLayout coordinates); CurrentMapDrawMetatileAt redraws it from
// the grid (Platform_OnMapCellRedrawn; -1, -1: the whole view).
void Platform_OnDoorTiles(int x, int y, const u16 *tiles);
void Platform_OnMapCellRedrawn(int x, int y);

#endif