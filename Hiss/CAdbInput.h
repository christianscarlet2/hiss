//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Route bot input into an adb-backed guest (scrcpy mirror) instead of
//   synthesising mouse events on the mirror window.
//
//   SendInput-style clicks need the scrcpy (SDL) window focused and topmost, which it
//   usually is NOT -- the bot runs with the mirror backgrounded, and on some mirrors
//   (notably the Android Studio AVD) the clicks never reach the guest at all. Injecting
//   over adb lands regardless of focus or occlusion, the same way the PrintWindow-based
//   scrape reads the screen without the window on top.
//
//   Device binding is fully automatic: the adb serial is read from the -s argument of
//   the scrcpy process that owns the window, so every scrcpy mirror and every Hiss
//   instance resolves its own device with no hardcoded table. Windows that are not
//   scrcpy mirrors report false and the caller falls back to the mouse DLL.
//
//******************************************************************************

#ifndef INC_CADBINPUT_H
#define INC_CADBINPUT_H

#include <windows.h>

namespace AdbInput {

// True if `window` is backed by an adb device. Cached, so this is cheap to call per action.
bool IsAdbBackedWindow(HWND window);

// Tap the centre of `rect` (client coords of `window`) `clicks` times.
// Returns false if the window is not adb-backed or the tap could not be sent.
bool TapRect(HWND window, RECT rect, int clicks);

// Swipe from (rect.left, rect.top) to (rect.right, rect.bottom) -- the same start/end
// encoding the mouse DLL's click-drag uses. `duration_ms` <= 0 picks a sane default.
// Returns false if the window is not adb-backed or the swipe could not be sent.
bool SwipeRect(HWND window, RECT rect, int duration_ms);

}  // namespace AdbInput

#endif  // INC_CADBINPUT_H
