//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Scraping the poker-table and providing access to the scraped data.
//  As the CScraper is low-level and quite large we created 
//  a interface SCraperAccess that provides higher-level accessors
//  like "UserHasCards()".
//  Better use that interface to access scraper-data whenever possible.
//
//******************************************************************************

#ifndef INC_BITMAPS_H
#define INC_BITMAPS_H

bool BitmapsAreEqual(HBITMAP HBitmapLeft, HBITMAP HBitmapRight) ;

// Tolerance-aware compare for change-detection on a noisy/jittery capture
// (phone mirror / scrcpy). Returns true when the two bitmaps are "effectively
// the same": a pixel only counts as changed when its largest per-channel
// difference exceeds pixel_delta, and the bitmaps are still considered equal
// unless MORE THAN min_changed_pixels such pixels exist. With pixel_delta <= 0
// it falls back to exact equality (identical to BitmapsAreEqual).
bool BitmapsAreSimilar(HBITMAP HBitmapLeft, HBITMAP HBitmapRight,
                       int pixel_delta, int min_changed_pixels);

#endif INC_BITMAPS_H
