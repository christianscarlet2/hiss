//*******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//*******************************************************************************
//
// Purpose:
//
//*******************************************************************************

#include "StdAfx.h"
#include "CTransform.h"

#include <Math.h>
#include "..\Shared\MagicNumbers\MagicNumbers.h"
#include "..\Hiss\NumericalFunctions.h"
#include "..\DLLs\Preferences_DLL\Preferences.h"
#include "..\DLLs\StringFunctions_DLL\string_functions.h"
#include "..\Hiss\WebSafeColours.h"

#ifdef OPENHOLDEM_PROGRAM
//include "../DLLs/Debug_DLL\debug.h"
#endif

#ifdef OPENSCRAPE_PROGRAM
#include "../Vision/debug.h"
#endif

CTransform::CTransform() {
}

CTransform::~CTransform() {
}

int CTransform::DoTransform(RMapCI region,
                  const HDC hdc, 
                  CString *text,
                  CString *separation,
                  COLORREF *cr_avg) {
	bool				character[MAX_CHAR_WIDTH][MAX_CHAR_HEIGHT] = {false};	
	bool				background[MAX_CHAR_WIDTH] = {false};

	// Initialize arrays
	for (int i=0; i<MAX_CHAR_WIDTH; i++)
		background[i] = true;


	switch (region->second.transform[0]) 
	{
		case 'C':
			return CTypeTransform(region, hdc, text, cr_avg);
			break;
		
		case 'I':
			return ITypeTransform(region, hdc, text);
			break;
		
		case 'H':
			return HTypeTransform(region, hdc, text);
			break;

		case 'T':
			return TTypeTransform(region, hdc, text, separation, background, character);
			break;

    case 'W':
			return WTypeTransform(region, hdc, text);
			break;

		case 'N':
			*text = "";
			return ERR_GOOD_SCRAPE_GENERAL;
			break;
	}

	return ERR_INVALID_TRANSFORM_TYPE;
}

// Average colour (each channel 0..255) of the width x height image in the bitmap
// currently selected into hdc. Returns false if the pixels can't be read.
static bool AverageColorOfDC(const HDC hdc, int width, int height,
							 double *out_r, double *out_g, double *out_b) {
	*out_r = *out_g = *out_b = 0.;
	if (hdc == NULL || width <= 0 || height <= 0) {
		return false;
	}
	BITMAPINFO *bmi = (BITMAPINFO *) ::HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		sizeof(BITMAPINFOHEADER) + 1024);
	HBITMAP hbm = (HBITMAP) GetCurrentObject(hdc, OBJ_BITMAP);
	bmi->bmiHeader.biSize = sizeof(bmi->bmiHeader);
	bmi->bmiHeader.biBitCount = 0;
	::GetDIBits(hdc, hbm, 0, 0, NULL, bmi, DIB_RGB_COLORS);
	bmi->bmiHeader.biHeight = -bmi->bmiHeader.biHeight;
	BYTE *pBits = new BYTE[bmi->bmiHeader.biSizeImage];
	::GetDIBits(hdc, hbm, 0, height, pBits, bmi, DIB_RGB_COLORS);

	long rr = 0, gg = 0, bb = 0, cnt = 0;
	for (int x = 0; x < width; x++) {
		for (int y = 0; y < height; y++) {
			rr += pBits[y*width*4 + x*4 + 2];
			gg += pBits[y*width*4 + x*4 + 1];
			bb += pBits[y*width*4 + x*4 + 0];
			cnt++;
		}
	}
	if (cnt != 0) {
		*out_r = (double)rr / cnt;
		*out_g = (double)gg / cnt;
		*out_b = (double)bb / cnt;
	}
	HeapFree(GetProcessHeap(), NULL, bmi);
	delete [] pBits;
	return true;
}

bool CTransform::AvgColorInAnyEnabledCube(RMapCI region, int r, int g, int b) {
	if (IsInRGBColorCube(GetRValue(region->second.color), GetGValue(region->second.color),
			GetBValue(region->second.color), region->second.radius, r, g, b)) {
		return true;
	}
	if (region->second.color2_enabled
		&& IsInRGBColorCube(GetRValue(region->second.color2), GetGValue(region->second.color2),
			GetBValue(region->second.color2), region->second.radius, r, g, b)) {
		return true;
	}
	if (region->second.color3_enabled
		&& IsInRGBColorCube(GetRValue(region->second.color3), GetGValue(region->second.color3),
			GetBValue(region->second.color3), region->second.radius, r, g, b)) {
		return true;
	}
	return false;
}

int CTransform::CTypeTransform(RMapCI region, const HDC hdc, CString *text, COLORREF *cr_avg) {
	// --- First rectangle: average its already-captured pixels (selected into hdc) ---
	int width  = region->second.right  - region->second.left + 1;
	int height = region->second.bottom - region->second.top  + 1;
	double rr_avg = 0., gg_avg = 0., bb_avg = 0.;
	AverageColorOfDC(hdc, width, height, &rr_avg, &gg_avg, &bb_avg);

	if (cr_avg != NULL) {
		*cr_avg = RGB((int)rr_avg, (int)gg_avg, (int)bb_avg);
	}

	// A region matches if the sampled average falls in ANY enabled colour cube
	// (colour 1, plus optionally colour 2 / colour 3 - same tolerance).
	bool in_cube = AvgColorInAnyEnabledCube(region, (int)rr_avg, (int)gg_avg, (int)bb_avg);

	// --- Optional second rectangle: if rect 1 didn't match, sample rect 2 (captured
	// into cur_bmp2 by the scraper) and test the same cubes. Result is true if ANY of
	// the (up to 6) colour/rectangle combinations match. ---
	if (!in_cube && region->second.rect2_enabled && region->second.cur_bmp2 != NULL) {
		int w2 = region->second.right2  - region->second.left2 + 1;
		int h2 = region->second.bottom2 - region->second.top2  + 1;
		if (w2 > 0 && h2 > 0) {
			HDC memdc = CreateCompatibleDC(hdc);
			if (memdc != NULL) {
				HBITMAP old_bmp = (HBITMAP) SelectObject(memdc, region->second.cur_bmp2);
				double r2 = 0., g2 = 0., b2 = 0.;
				AverageColorOfDC(memdc, w2, h2, &r2, &g2, &b2);
				SelectObject(memdc, old_bmp);
				DeleteDC(memdc);
				in_cube = AvgColorInAnyEnabledCube(region, (int)r2, (int)g2, (int)b2);
			}
		}
	}

	*text = in_cube ? "true" : "false";
	return ERR_GOOD_SCRAPE_GENERAL;
}

int CTransform::ITypeTransform(RMapCI region, const HDC hdc, CString *text) 
{
	int					width = 0, height = 0, x = 0, y = 0, i = 0, result = 0;
	IMapCI				best_match = p_tablemap->i$()->end();
	unsigned int		smallest_pix_diff = 0;
	int					retval=ERR_NOTHING_TO_SCRAPE;
	HBITMAP				hbm = NULL;
	BYTE				*pBits = NULL, alpha = 0, red = 0, green = 0, blue = 0;
	CompareArgs			args;
	CString				s = "";


	// Allocate heap space for BITMAPINFO
	BITMAPINFO	*bmi = NULL;
	int			info_len = sizeof(BITMAPINFOHEADER) + 1024;
	bmi = (BITMAPINFO *) ::HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, info_len);

	width = region->second.right - region->second.left + 1;
	height = region->second.bottom - region->second.top + 1;
	
	// See if region size is too large
	if (width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT) 
	{
		HeapFree(GetProcessHeap(), NULL, bmi);
		return ERR_FIELD_TOO_LARGE;
	}

	// If width or height is negative (left>right or bottom>top), then return
	if (width<0 || height<0)
	{
		HeapFree(GetProcessHeap(), NULL, bmi);
		return ERR_NOTHING_TO_SCRAPE;
	}

	// Get pixels
	// Populate BITMAPINFOHEADER
	hbm = (HBITMAP) GetCurrentObject(hdc, OBJ_BITMAP);
	bmi->bmiHeader.biSize = sizeof(bmi->bmiHeader);
	bmi->bmiHeader.biBitCount = 0;
	::GetDIBits(hdc, hbm, 0, 0, NULL, bmi, DIB_RGB_COLORS);

	// Get the actual argb bit information
	bmi->bmiHeader.biHeight = -bmi->bmiHeader.biHeight;
	pBits = new BYTE[bmi->bmiHeader.biSizeImage];
	::GetDIBits(hdc, hbm, 0, height, pBits, bmi, DIB_RGB_COLORS);


	// load ImgA with current pixels from region
	args.ImgA = new RGBAImage(width, height, "imga.ppm");
	for (y = 0; y < height; y++) 
	{
		for (x = 0; x < width; x++) 
		{
			alpha = pBits[y*width*4 + x*4 + 3];
			red = pBits[y*width*4 + x*4 + 2];
			green = pBits[y*width*4 + x*4 + 1];
			blue = pBits[y*width*4 + x*4 + 0];
			args.ImgA->Set(red, green, blue, alpha, x + y * width);
		}
	}

	// Scan ALL same-size i$ records and keep the pixel-closest one (nearest-neighbour by
	// the summed absolute channel difference). NOT the Yee perceptual metric, which is
	// built for photographic difference and ranks tiny clean card glyphs badly (it would
	// report e.g. "3d" closer than "Qd").
	unsigned int best_total_diff = 0xffffffff;   // summed |dR|+|dG|+|dB| over the region
	unsigned int best_blank_diff = 0xffffffff;   // closest "no card" (empty-named) template
	int dim = width * height;
	for (IMapCI i_iter=p_tablemap->i$()->begin(); i_iter!=p_tablemap->i$()->end(); i_iter++)
	{
		if (i_iter->second.width != width || i_iter->second.height != height
			|| i_iter->second.image == NULL)
			continue;
		RGBAImage *imgB = i_iter->second.image;

		unsigned int total = 0;
		for (int k = 0; k < dim; k++) {
			int dr = (int)args.ImgA->Get_Red(k)   - (int)imgB->Get_Red(k);
			int dg = (int)args.ImgA->Get_Green(k) - (int)imgB->Get_Green(k);
			int db = (int)args.ImgA->Get_Blue(k)  - (int)imgB->Get_Blue(k);
			total += (unsigned int)((dr < 0 ? -dr : dr) + (dg < 0 ? -dg : dg) + (db < 0 ? -db : db));
		}
		if (total < best_total_diff) {
			best_match = i_iter;
			best_total_diff = total;
		}
		// An image with an empty name is a "no card / blank slot" template.
		if (i_iter->second.name.IsEmpty() && total < best_blank_diff) {
			best_blank_diff = total;
		}
	}

	// ImgB was never owned by args here; make sure its destructor won't touch it.
	args.ImgB = NULL;

	double avg       = (dim > 0) ? (double)best_total_diff / (double)(dim * 3) : 1e9;
	double blank_avg = (dim > 0) ? (double)best_blank_diff / (double)(dim * 3) : 1e9;

	// Average per-channel difference (0..255), resolution-independent so a 10x10 suit and
	// a 22x39 rank are judged the same way.
	//   kBlankAvgDiff:  if the region matches a known blank/"no card" template this closely,
	//                   it IS a blank -- report no card. This guards against tablemaps that
	//                   accidentally saved the empty slot under a card name (e.g. a blank
	//                   captured as "Qd"): a real card face differs from a blank by ~150+,
	//                   so this never suppresses a genuine card.
	//   kAcceptAvgDiff: otherwise accept the pixel-closest card image if it's close enough.
	const double kBlankAvgDiff  = 40.0;
	const double kAcceptAvgDiff = 90.0;
	if (best_blank_diff != 0xffffffff && blank_avg < kBlankAvgDiff)
	{
		*text = "";                          // matched a blank slot -> no card
		retval = ERR_GOOD_SCRAPE_GENERAL;
	}
	else if (best_match != p_tablemap->i$()->end() && avg < kAcceptAvgDiff)
	{
		*text = best_match->second.name.GetString();
		retval = ERR_GOOD_SCRAPE_GENERAL;
	}
	else
	{
		*text = "";
		retval = ERR_NO_IMAGE_MATCH;
	}

	// Clean up
	HeapFree(GetProcessHeap(), NULL, bmi);
	delete []pBits;

	return retval;
}

int CTransform::WTypeTransform(RMapCI region, const HDC hdc, CString *text) {
  // Just take the colour of the pixel in the middle of the region
  int x_pos = (region->second.right - region->second.left) / 2;
  int y_pos = (region->second.bottom - region->second.top) / 2;
  int colour = GetPixel(hdc, x_pos, y_pos); 
  int web_safe_colour = ClosestWebSafeColour(colour);
  // Now transform the result to text, 
  // as that's our general format for scraped values
  text->Format("%i", web_safe_colour);
  return ERR_GOOD_SCRAPE_GENERAL;
}

int CTransform::HTypeTransform(RMapCI region, const HDC hdc, CString *text) 
{
	int					x = 0, y = 0, j = 0;
	int					width = 0, height = 0;
	int					hash_type = 0, pixcount = 0;
	uint32_t		hash = 0, pix[MAX_HASH_WIDTH*MAX_HASH_HEIGHT] = {0};
	int					retval = ERR_NOTHING_TO_SCRAPE;
	BYTE				*pBits = NULL, red = 0, green = 0, blue = 0;

	width = region->second.right - region->second.left + 1;
	height = region->second.bottom - region->second.top + 1;
	hash_type = RightDigitCharacterToNumber(region->second.transform);
	
	if (p_tablemap->h$(hash_type)->empty())
		return ERR_NO_HASH_MATCH;

	// See if region size is too large
	if (width > MAX_HASH_WIDTH || height > MAX_HASH_HEIGHT) 
		return ERR_FIELD_TOO_LARGE;

	// If width or height is negative (left>right or bottom>top), then return
	if (width<0 || height<0)
		return ERR_NOTHING_TO_SCRAPE;

	// Allocate heap space for BITMAPINFO
	BITMAPINFO	*bmi;
	int			info_len = sizeof(BITMAPINFOHEADER) + 1024;
	bmi = (BITMAPINFO *) ::HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, info_len);

	// Get pixels
	// Populate BITMAPINFOHEADER
	HBITMAP hbm = (HBITMAP) GetCurrentObject(hdc, OBJ_BITMAP);
	bmi->bmiHeader.biSize = sizeof(bmi->bmiHeader);
	bmi->bmiHeader.biBitCount = 0;
	::GetDIBits(hdc, hbm, 0, 0, NULL, bmi, DIB_RGB_COLORS);

	// Get the actual argb bit information
	bmi->bmiHeader.biHeight = -bmi->bmiHeader.biHeight;
	pBits = new BYTE[bmi->bmiHeader.biSizeImage];
	::GetDIBits(hdc, hbm, 0, height, pBits, bmi, DIB_RGB_COLORS);

	// all pixel hash
	if (hash_type == 0) 
	{
		for (y = 0; y < height; y++) 
		{
			for (x = 0; x < width; x++) 
			{
				red = pBits[y*width*4 + x*4 + 2];
				green = pBits[y*width*4 + x*4 + 1];
				blue = pBits[y*width*4 + x*4 + 0];
				pix[y*width + x] = (blue<<16) + (green<<8) + red;
			}
		}
		hash = hashword(&pix[0], width * height, HASH_SEED_0);
	}

	// selected pixel hash
	else if (hash_type>=1 && hash_type<k_max_number_of_hash_groups_in_tablemap) 
	{
		pixcount = 0;
		for (PMapCI p_iter=p_tablemap->p$(hash_type)->begin(); p_iter!=p_tablemap->p$(hash_type)->end(); p_iter++) 
		{
			x = p_iter->second.x;
			y = p_iter->second.y;

			if (x<width && y<height)
			{
				red = pBits[y*width*4 + x*4 + 2];
				green = pBits[y*width*4 + x*4 + 1];
				blue = pBits[y*width*4 + x*4 + 0];
				pix[pixcount++] = (blue<<16) + (green<<8) + red;
			}
		}

		if (hash_type==1)  hash = hashword(&pix[0], pixcount, HASH_SEED_1);
		if (hash_type==2)  hash = hashword(&pix[0], pixcount, HASH_SEED_2);
		if (hash_type==3)  hash = hashword(&pix[0], pixcount, HASH_SEED_3);
	}

	// lookup hash in h$ records
	HMapCI h_iter = p_tablemap->h$(hash_type)->find(hash);

	// no hash match
	if (h_iter == p_tablemap->h$(hash_type)->end()) 
	{ 
		retval = ERR_NO_HASH_MATCH; 
		*text = "";
	}
	// hash match found
	else 
	{ 
		retval = ERR_GOOD_SCRAPE_GENERAL; 
		*text = h_iter->second.name;
	}

	// Clean up
	HeapFree(GetProcessHeap(), NULL, bmi);
	delete []pBits;

	return retval;
}

// Debug-only: compute the hash value HTypeTransform would derive for this region's
// current pixels and hash_type, without doing the h$ lookup. Kept in lock-step with the
// hashing in HTypeTransform so the reported value matches what detection actually uses.
uint32_t CTransform::ComputeRegionHash(RMapCI region, const HDC hdc, int hash_type)
{
	if (hash_type < 0 || hash_type >= k_max_number_of_hash_groups_in_tablemap) return 0;
	int width  = region->second.right - region->second.left + 1;
	int height = region->second.bottom - region->second.top + 1;
	if (width <= 0 || height <= 0 || width > MAX_HASH_WIDTH || height > MAX_HASH_HEIGHT) {
		return 0;
	}
	uint32_t pix[MAX_HASH_WIDTH*MAX_HASH_HEIGHT] = {0};
	BITMAPINFO *bmi = (BITMAPINFO *) ::HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		sizeof(BITMAPINFOHEADER) + 1024);
	if (bmi == NULL) return 0;
	HBITMAP hbm = (HBITMAP) GetCurrentObject(hdc, OBJ_BITMAP);
	bmi->bmiHeader.biSize = sizeof(bmi->bmiHeader);
	bmi->bmiHeader.biBitCount = 0;
	::GetDIBits(hdc, hbm, 0, 0, NULL, bmi, DIB_RGB_COLORS);
	bmi->bmiHeader.biHeight = -bmi->bmiHeader.biHeight;
	BYTE *pBits = new BYTE[bmi->bmiHeader.biSizeImage];
	::GetDIBits(hdc, hbm, 0, height, pBits, bmi, DIB_RGB_COLORS);

	uint32_t hash = 0;
	if (hash_type == 0) {
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				BYTE red   = pBits[y*width*4 + x*4 + 2];
				BYTE green = pBits[y*width*4 + x*4 + 1];
				BYTE blue  = pBits[y*width*4 + x*4 + 0];
				pix[y*width + x] = (blue<<16) + (green<<8) + red;
			}
		}
		hash = hashword(&pix[0], width * height, HASH_SEED_0);
	} else {
		int pixcount = 0;
		for (PMapCI p_iter=p_tablemap->p$(hash_type)->begin(); p_iter!=p_tablemap->p$(hash_type)->end(); p_iter++) {
			int x = p_iter->second.x, y = p_iter->second.y;
			if (x < width && y < height) {
				BYTE red   = pBits[y*width*4 + x*4 + 2];
				BYTE green = pBits[y*width*4 + x*4 + 1];
				BYTE blue  = pBits[y*width*4 + x*4 + 0];
				pix[pixcount++] = (blue<<16) + (green<<8) + red;
			}
		}
		if (hash_type==1) hash = hashword(&pix[0], pixcount, HASH_SEED_1);
		if (hash_type==2) hash = hashword(&pix[0], pixcount, HASH_SEED_2);
		if (hash_type==3) hash = hashword(&pix[0], pixcount, HASH_SEED_3);
	}
	HeapFree(GetProcessHeap(), NULL, bmi);
	delete []pBits;
	return hash;
}


int CTransform::TTypeTransform(RMapCI region, const HDC hdc, CString *text, CString *separation,
									 bool background[], bool (*character)[MAX_CHAR_HEIGHT])
{
	int					x = 0, y = 0;
	int					i = 0;
	int					width = region->second.right - region->second.left + 1;
	int					height = region->second.bottom - region->second.top + 1;
	CString				s = "", s$tXtype = "";
	BYTE				*pBits = NULL, alpha = 0, red = 0, green = 0, blue = 0;

	// Initialize
	*text = "";

	// Check max size
	if (width > MAX_CHAR_WIDTH) 
	{
		text->Append("Field too wide");
		return ERR_FIELD_TOO_LARGE;
	}
	if (height > MAX_CHAR_HEIGHT)
	{
		text->Append("Field too tall");
		return ERR_FIELD_TOO_LARGE;
	}
	// Guard a malformed/zero region (a transient table-relayout frame can give right<left or
	// bottom<top -> width/height <= 0), which would run the pixel loop off the buffer below.
	if (width <= 0 || height <= 0)
	{
		return ERR_FIELD_TOO_LARGE;
	}

	// Get associated s$record
	s$tXtype = "plain";

	s.Format("t%ctype", region->second.transform[1]);

	SMapCI s_iter = p_tablemap->s$()->find(s.GetString());
	if (s_iter != p_tablemap->s$()->end())
		s$tXtype = s_iter->second.text;

	// Allocate heap space for BITMAPINFO
	BITMAPINFO	*bmi = NULL;
	int			info_len = sizeof(BITMAPINFOHEADER) + 1024;
	bmi = (BITMAPINFO *) ::HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, info_len);

	// Load entire foreground pixel array into memory
	// Populate BITMAPINFOHEADER
	HBITMAP hbm = (HBITMAP) GetCurrentObject(hdc, OBJ_BITMAP);
	bmi->bmiHeader.biSize = sizeof(bmi->bmiHeader);
	bmi->bmiHeader.biBitCount = 0;
	::GetDIBits(hdc, hbm, 0, 0, NULL, bmi, DIB_RGB_COLORS);

	// Get the actual argb bit information
	bmi->bmiHeader.biHeight = -bmi->bmiHeader.biHeight;
	pBits = new BYTE[bmi->bmiHeader.biSizeImage];
	::GetDIBits(hdc, hbm, 0, height, pBits, bmi, DIB_RGB_COLORS);

	// Guard: when scrcpy/the attached window isn't really there (e.g. closed, or mid table
	// re-layout), the capture yields a biSizeImage smaller than width*height*4 (or 0).
	// Indexing pBits in the loop below then runs off the buffer -- the access violation that
	// crashed Hiss in CTransform::TTypeTransform. Read the region as empty this frame instead.
	if (pBits == NULL || bmi->bmiHeader.biSizeImage < (DWORD)(width * height * 4))
	{
		::HeapFree(GetProcessHeap(), NULL, bmi);
		delete [] pBits;
		return ERR_FIELD_TOO_LARGE;
	}

	for (x = 0; x < width; x++)
	{
		for (y = 0; y < height; y++) 
		{
			alpha = pBits[y*width*4 + x*4 + 3];
			red = pBits[y*width*4 + x*4 + 2];
			green = pBits[y*width*4 + x*4 + 1];
			blue = pBits[y*width*4 + x*4 + 0];

			// See if our pixel is in the defined color cube
			if (IsInARGBColorCube((region->second.color>>24)&0xff,
								 GetRValue(region->second.color), 
								 GetGValue(region->second.color), 
								 GetBValue(region->second.color), 
								 region->second.radius, 
								 alpha, 
								 red, 
								 green,
								 blue))
			{
				character[x][y] = true;
			}

			// Set flag if this is a not a completely background band
			if (character[x][y] != false) 
				background[x] = false;

		}
	}

	// Clean up
	HeapFree(GetProcessHeap(), NULL, bmi);
	delete []pBits;

	// Save separation information
	if (separation != NULL)
	{
		*separation = "";
		for (y=0; y<height; y++) 
		{
			for (x=0; x<width; x++) 
				separation->Append(character[x][y] ? "X" : " ");

			separation->Append("\r\n");
		}
	}

	// scan through OCR region - plain
	if (s$tXtype.MakeLower() != "fuzzy" && atof(s$tXtype.GetString()) == 0)
		return DoPlainFontScan(region, width, height, background, character, text);

	// scan through OCR region - fuzzy
	else 
		return DoFuzzyFontScan(region, width, height, background, character, 
								  s$tXtype.MakeLower()=="fuzzy" ? DEFAULT_FUZZY_TOLERANCE : atof(s$tXtype.GetString()) ,
								  text);
}

int CTransform::DoPlainFontScan(RMapCI region, const int width, const int height, 
									  const bool bg[], const bool (*ch)[MAX_CHAR_HEIGHT], CString *text)
{
	int					y = 0, retval=ERR_TEXT_SCRAPE_NOMATCH;
	int					x_begin = 0, x_end = 0, y_begin = 0, y_end = 0;
	int					temp_right = 0, vert_band_left = 0;
	bool				backg_band = false;
	CString				newchar = "", hexmash = "";
	int					text_group = atoi(region->second.transform.Right(1));
	std::map<CString, int>::const_iterator	fontindex;

	// Skip any background bands
	backg_band = true;

	while (vert_band_left<width && backg_band==true) 
	{
		for (y = 0; y<=height; y++) 
		{
			if (ch[vert_band_left][y]) 
			{
					backg_band = false;
					y = height + 1;
			}
		}
		if (backg_band==true) 
		{
			vert_band_left++;
		}
	}

	while (vert_band_left<width) 
	{


		// Shift left and down to eliminate white space on left and bottom, starting at 
		// vert_band_left and extending MAX_SINGLE_CHAR_WIDTH pixels to the right
		GetShiftLeftDownIndexes(vert_band_left, MAX_SINGLE_CHAR_WIDTH, height, bg, ch, &x_begin, &x_end, &y_begin, &y_end);

		if (y_end-y_begin > MAX_SINGLE_CHAR_HEIGHT)
			y_begin = y_end - MAX_SINGLE_CHAR_HEIGHT;

		// scanning right to left to find the largest match
		// start at vert_band_left+MAX_SINGLE_CHAR_WIDTH
		temp_right = vert_band_left + MAX_SINGLE_CHAR_WIDTH;

		if (temp_right>width)
			temp_right = width;

		bool continue_looping = true;
		do 
		{
			// calculate hexmash
			CalcHexmash(vert_band_left, temp_right, y_begin, y_end, ch, &hexmash);

			// lookup font in t$ records
			TMapCI t_iter = p_tablemap->t$(text_group)->find(hexmash.GetString());

			// Found match, save char and move on
			if (t_iter != p_tablemap->t$(text_group)->end()) 
			{
				retval = ERR_GOOD_SCRAPE_GENERAL;

				newchar.Format("%c", t_iter->second.ch);
				text->Append(newchar);

				vert_band_left = temp_right + 1;

				continue_looping = false;
			}

			// No match:  reduce right hand size, or report '?' or we are out of space
			else 
			{

				if (temp_right>vert_band_left) 
				{
					temp_right--;
				}
				else 
				{
					// we've scraped something that can't be recognized
					vert_band_left++;

					continue_looping = false;
				}
			}
		} while (continue_looping);
	} // while (vert_band_left<width)

	return retval;
}

int CTransform::DoFuzzyFontScan(RMapCI region, const int width, const int height, 
									  const bool bg[], const bool (*ch)[MAX_CHAR_HEIGHT], const double tolerance, 
									  CString *text)
{

	int					y = 0, t_index = 0, retval = ERR_TEXT_SCRAPE_NOMATCH;
	int					vert_band_left = 0;
	bool				backg_band = false;
	CString				newchar = "";
	int					font_group = strtoul(CString(region->second.transform[1]), NULL, 10);

	while (vert_band_left<width) 
	{

		// Skip any background bands
		backg_band = true;
		while (vert_band_left<width && backg_band==true) 
		{
			for (y = 0; y<=height; y++) 
			{
				if (ch[vert_band_left][y]) 
				{
						backg_band = false;
						y = height + 1;
				}
			}
			if (backg_band==true) 
			{
				vert_band_left++;
			}
		}

		// Find best hamming distance match within this MAX_SINGLE_CHAR_WIDTH group of bands 
		TMapCI t_iter = GetBestHammingDistance(region, width, height, bg, ch, vert_band_left, tolerance);

		if (t_iter != p_tablemap->t$(font_group)->end())
		{
			retval = ERR_GOOD_SCRAPE_GENERAL;
			
			newchar.Format("%c", t_iter->second.ch);
			text->Append(newchar);

			vert_band_left = vert_band_left + t_iter->second.x_count;
		}
		else
		{
			vert_band_left += 1;
		}
	}

	return retval;
}
		
TMapCI CTransform::GetBestHammingDistance(RMapCI region, const int width, const int height, 
											 const bool bg[], const bool (*ch)[MAX_CHAR_HEIGHT], 
											 const int left, const double tolerance)
{
	CString				hex_bits = "";
	double				best_weighted_hd = 999999.;
	unsigned int		hexval_array[MAX_SINGLE_CHAR_WIDTH] = {0};
	int					font_group = strtoul(CString(region->second.transform[1]), NULL, 10);
	TMapCI				best_hd_t_iter = p_tablemap->t$(font_group)->end();

	for (int x=0; x<MAX_SINGLE_CHAR_WIDTH && left+x<width; x++)
	{
		// Shift left and down to eliminate white space on left and bottom
		int x_begin = 0, x_end = 0, y_begin = 0, y_end = 0;
		GetShiftLeftDownIndexes(left, x+1, height, bg, ch, &x_begin, &x_end, &y_begin, &y_end);

		if (y_end-y_begin > MAX_SINGLE_CHAR_HEIGHT)
			y_begin = y_end - MAX_SINGLE_CHAR_HEIGHT;

		// Get array of hexvals
		for (int i=0; i<MAX_SINGLE_CHAR_WIDTH; i++)
			hexval_array[i] = 0;

		for (int i=x_begin; i<=x_end; i++)
		{
			for (int y=y_end; y>=y_begin; y--)
				if (ch[i][y])
					hexval_array[i-x_begin] += (1 << (y_end-y));
		}

		// Scan through each font record for this region's group and identify the one with the 
		// best hamming distance match
		for (TMapCI t_iter=p_tablemap->t$(font_group)->begin(); t_iter!=p_tablemap->t$(font_group)->end(); t_iter++)
		{
			double lit_pixels = 0.000001;
			double tot_hd = 0.000001;
			double weighted_hd = 999999.;

			// If the size of this character is <= the number of columns we are looking at then consider it
			if (t_iter->second.x_count<=x+1)
			{
				// Get the hamming distance and lit pix counts "by column" left to right
				for (int j=0; j<t_iter->second.x_count && j<x+1; j++)
				{
					tot_hd += CalcHammingDistance(t_iter->second.x[j], hexval_array[j]);
					lit_pixels += bitcount(t_iter->second.x[j]);
				}

				weighted_hd = (double) tot_hd / (double) lit_pixels;
			}

			// Keep track of our best match - skip this if weighted_hd exceeds the tolerance
			if (weighted_hd < tolerance && weighted_hd < best_weighted_hd)
			{
				best_hd_t_iter = t_iter;
				best_weighted_hd = weighted_hd;
				if (tot_hd > lit_pixels) break;
			}
		}
	}

	return best_hd_t_iter;
}

int CTransform::CalcHammingDistance(const unsigned int x, const unsigned int y)
{
	int dist = 0, val = x ^ y;

	while(val)
	{
		++dist; 
		val &= val - 1;
	}

	return dist;
}


void CTransform::GetShiftLeftDownIndexes(const int x_start, const int width, const int height, 
											   const bool background[], const bool character[][MAX_CHAR_HEIGHT], 
											   int *x_begin, int *x_end, int *y_begin, int *y_end)
{
	int	x = 0, y = 0;

	// Find the real beginning and end on the x dimension
	*x_begin = x_start+width-1;
	*x_end = x_start;
	for (x=x_start; x<x_start+width; x++) 
	{
		if (!background[x]) 
		{
			*x_begin = x;
			x = x_start + width + 1;
		}
	}

	for (x=x_start+width-1; x>=x_start; x--) 
	{
		if (!background[x]) 
		{
			*x_end = x;
			x = -1;
		}
	}

	// Find the real beginning and end on the y dimension
	*y_begin = height-1;
	*y_end = 0;
	for (y=0; y<height; y++) 
	{
		for (x=*x_begin; x<=*x_end; x++) 
		{
			if (character[x][y]) 
			{
				*y_begin = y;
				x = *x_end + 1;
				y = height + 1;
			}
		}
	}

	for (y=height-1; y>=0; y--) 
	{
		for (x=*x_begin; x<=*x_end; x++) 
		{
			if (character[x][y]) 
			{
				*y_end = y;
				x = *x_end + 1;
				y = -1;
			}
		}
	}
}

bool CTransform::IsInARGBColorCube(const int center_a, const int center_r, const int center_g, const int center_b, 
									     const int radius, const int pix_a, const int pix_r, const int pix_g, 
										 const int pix_b) 
{
	int		a_diff = 0, r_diff = 0, g_diff = 0, b_diff = 0, tot_diff = 0;

	// zero radius
	if (radius == 0) 
	{
		if (pix_a == center_a && pix_r == center_r && pix_g == center_g && pix_b == center_b)
			return true;
	}

	// non-zero radius
	else 
	{
		a_diff = center_a - pix_a;
		r_diff = center_r - pix_r; 
		g_diff = center_g - pix_g; 
		b_diff = center_b - pix_b; 
		tot_diff = (int) pow((double) (a_diff*a_diff + r_diff*r_diff + g_diff*g_diff + b_diff*b_diff), 0.5);

		// positive radius
		if (radius >= 0)
		{
			if (tot_diff <= radius) 
				return true;
		}

		// negative radius (logical not)
		else 
		{
			if (tot_diff > -radius)
				 return true;
		}
	}	

	return false;
}

bool CTransform::IsInRGBColorCube(const int center_r, const int center_g, const int center_b, 
										const int radius, const int pix_r, const int pix_g, const int pix_b)
{
	int		r_diff = 0, g_diff = 0, b_diff = 0, tot_diff = 0;

	// zero radius
	if (radius == 0) 
	{
		if (pix_r == center_r && pix_g == center_g && pix_b == center_b)
			return true;
	}

	// non-zero radius
	else 
	{
		r_diff = center_r - pix_r; 
		g_diff = center_g - pix_g; 
		b_diff = center_b - pix_b; 
		tot_diff = (int) pow((double) (r_diff*r_diff + g_diff*g_diff + b_diff*b_diff), 0.5);

		// positive radius
		if (radius >= 0) 
		{
			if (tot_diff <= radius)
				return true;
		}

		// negative radius (logical not)
		else 
		{
			if (tot_diff > -radius)
				 return true;
		}
	}	

	return false;
}

void CTransform::CalcHexmash(const int left, const int right, const int top, const int bottom, 
								   const bool (*ch)[MAX_CHAR_HEIGHT], CString *hexmash, const bool withspace) {
	int				x = 0, y = 0, last_fg_row = 0;
	unsigned int	hexval = 0;
	char			t[20] = {0};
#ifdef OPENHOLDEM_PROGRAM
  write_log(Preferences()->debug_alltherest(),
    "[CTransform] CalcHexmash(%i, %i, %i, %i)\n",
    left, right, top, bottom);
#endif

	// find last horizontal row with foreground pixels
	last_fg_row = -1;
	for (y = bottom; y>=top; y--) 
	{
		for (x = left; x<= right; x++) 
		{
			if (ch[x][y]) 
			{
				last_fg_row = y;
				x = right + 1;
				y = -1;
			}
		}
	}

	// Calculate hexmash
	*hexmash = "";
	for (x = left; x <= right; x++) 
	{
		hexval = 0;
		for (y = last_fg_row; y>=top; y--)
			if (ch[x][y])
				hexval += (1 << (last_fg_row - y));

		sprintf_s(t, 20, "%x", hexval);

		*hexmash += t;
		if (withspace)  
			*hexmash += " ";
	}
}

