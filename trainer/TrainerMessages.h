#pragma once

#include <windows.h>

// Cross-thread signals from the HTTP server thread to the main dialog (UI thread).
// The server posts these; the dialog handles them (must run on the UI thread
// because they touch GDI capture / create windows).
#define WM_TRAINER_OPEN_FONTS    (WM_APP + 2)   // open the font-creation window
#define WM_TRAINER_CAPTURE_FONTS (WM_APP + 3)   // capture+segment the live scrapes into the glyph store

// Main dialog HWND, published once the dialog initializes (NULL before then).
extern HWND g_trainer_main_hwnd;
