#ifndef GUARD_PLATFORM_TEST_HOOKS_H
#define GUARD_PLATFORM_TEST_HOOKS_H

#include <stdbool.h>
#include <stdint.h>

// Debug shortcuts for automated tests, driven by environment variables
// (see test_hooks.c). They do nothing unless those variables are set.
void TestHooks_Init(void);
void TestHooks_OnFrame(uint32_t frame);

// PE_TEST_TOUR: name of the overview picture to take this frame (NULL: none),
// and whether the tour is over.
const char *TestHooks_OverviewWanted(void);
int TestHooks_OverviewPixelsPerBlock(void);
bool TestHooks_TourShots(void); // also save the game screen with each overview
const char *TestHooks_GameShotWanted(void); // name for <name>_game.png this frame, or NULL
bool TestHooks_Finished(void);

#endif // GUARD_PLATFORM_TEST_HOOKS_H
