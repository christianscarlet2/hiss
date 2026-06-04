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
// progress bar + Cancel). Reports setup errors via a message box.
void T2I_GenerateInteractive(CWnd *parent);
