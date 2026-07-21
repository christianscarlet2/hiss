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

#include <string>

// Given a top-down 24bpp BGR sub-image (one region's pixels) with the given
// row stride in bytes, returns the x (in that sub-image's pixel coordinates) of
// the decimal separator's centre, or -1 if no decimal separator was found.
int FindDecimalSplitX(const unsigned char *bgr, int width, int height, int stride);

// Locate the decimal dot in the image and count the digit glyphs to its right,
// stopping at the first large gap (e.g. before a trailing " BB"). Returns the
// number of decimal places (1 or 2) or 0 if no decimal point was found. Used to
// re-insert a decimal that whole-image OCR dropped (e.g. image "50.28" but OCR
// returned "5028").
int DetectDecimalPlaces(const unsigned char *bgr, int width, int height, int stride);

// Post-processing decimal correction. If `result` has no '.' already, detect the
// number of decimal places from the image and insert a '.' that many places from
// the right of `result` (padding a leading zero when needed, e.g. "7" -> "0.07").
// Returns `result` unchanged when no decimal is detected or it already has one.
std::string ApplyDecimalCorrection(const std::string &result,
		const unsigned char *bgr, int width, int height, int stride);
