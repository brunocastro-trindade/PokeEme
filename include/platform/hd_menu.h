#ifndef GUARD_PLATFORM_HD_MENU_H
#define GUARD_PLATFORM_HD_MENU_H

#include <stdbool.h>
#include <stdint.h>

// The HD-2D options menu, drawn over the game in the game's own font.
// Opens with Tab (keyboard) or ZL (controller); while it is open the game
// gets no buttons and keeps running, so every change shows at once.

#define HDMENU_WIDTH  240
#define HDMENU_HEIGHT 160

// Buttons the menu understands (held this frame)
#define HDMENU_UP      0x01
#define HDMENU_DOWN    0x02
#define HDMENU_LEFT    0x04
#define HDMENU_RIGHT   0x08
#define HDMENU_CONFIRM 0x10
#define HDMENU_BACK    0x20
#define HDMENU_OPEN    0x40 // opens and closes

// Once per presented frame, with the buttons held and the frame rate
void HdMenu_Update(uint32_t held, float fps);

bool HdMenu_IsOpen(void);
void HdMenu_Open(void);

// True while the game must not see the buttons (menu open, or buttons still
// held from closing it)
bool HdMenu_BlocksGameInput(void);

// A 240x160 RGBA image to lay over the game frame (menu and/or frame
// counter), or NULL when there is nothing to show. *version changes when
// the image changes.
const uint8_t *HdMenu_Overlay(uint32_t *version);

#endif // GUARD_PLATFORM_HD_MENU_H
