//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Decimal-separator locator for the Vision scrape view. Ported from
//   trainer's CTrainerOcr::FindDecimalSplit so Vision can draw a red line where
//   the decimal split happens. OpenCV is isolated to the .cpp.
//
//******************************************************************************

#pragma once

// Given a top-down 24bpp BGR sub-image (one region's pixels) with the given
// row stride in bytes, returns the x (in that sub-image's pixel coordinates) of
// the decimal separator's centre, or -1 if no decimal separator was found.
int FindDecimalSplitX(const unsigned char *bgr, int width, int height, int stride);
