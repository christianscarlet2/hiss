#pragma once

#include <afxwin.h>

// Username-sample generation tool (Tools menu + "Generate Username Samples" button on
// the main window). Renders PokerTracker-4 player names with Tesseract's text2image.exe
// using a random font weight from training\fonts, recolours them with the last Create-
// Fonts colours from the database (green text on a dark background), sizes them to match
// the existing training samples, and writes sample_NNNN.png + sample_NNNN.gt.txt into
// training\ without clashing with existing files.

// Open the modal settings dialog (text2image.exe path + PT4 connection). Persisted in
// the hiss `settings` table under key 'text2image'.
void T2I_OpenSettings(CWnd *parent);

// Ask "how many samples?", then run generation behind a modal progress dialog (with a
// progress bar + Cancel). Reports setup errors via a message box. Both generators
// distribute the work across the shared parallel worker pool.
void T2I_GenerateInteractive(CWnd *parent);          // PT4 player-name samples

// Same flow, but synthesises balance numbers (0-1000, with 0/1/2 decimal places) instead
// of pulling names from PT4. Shows an explanatory prompt before starting.
void T2I_GenerateBalancesInteractive(CWnd *parent);
