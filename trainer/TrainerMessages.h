#pragma once

#include <windows.h>

// Cross-thread signals from the HTTP server thread to the main dialog (UI thread).
// The server posts these; the dialog handles them (must run on the UI thread
// because they touch GDI capture / create windows).
#define WM_TRAINER_OPEN_FONTS    (WM_APP + 2)   // open the font-creation window
#define WM_TRAINER_CAPTURE_FONTS (WM_APP + 3)   // capture+segment the live scrapes into the glyph store
#define WM_TRAINER_RECOGNIZE_ALL (WM_APP + 4)   // re-recognize every table row with the current transform
// wParam: 0=status, 1=start, 2=stop, 3=toggle. Returns 1 if capturing afterwards, else 0.
#define WM_TRAINER_SET_CAPTURE   (WM_APP + 6)   // start/stop sample capture from the web UI
// wParam: glyph id. lParam: (mode<<8)|index. Returns the recognized char code (0 = none).
#define WM_TRAINER_OCR_GLYPH     (WM_APP + 7)   // OCR a font glyph's reference image (fonts editor)

// Transform modes stored in the sample store (which engine recognizes the table).
#define TRAINER_MODE_AUTOOCR 0                  // Tesseract (AutoOcr0/AutoOcr1)
#define TRAINER_MODE_TEXT    1                  // bitmap-font hashing (Text0..Text9)

// Main dialog HWND, published once the dialog initializes (NULL before then).
extern HWND g_trainer_main_hwnd;
