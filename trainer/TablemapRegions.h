#pragma once

#include <vector>

// A single tablemap region we care about (the balance fields).
struct STrainerRegion {
	CString  name;
	RECT     rect;       // left/top/right/bottom in client pixels
	COLORREF color;      // ARGB colour cube centre (alpha in high byte)
	int      radius;     // colour cube radius
	CString  transform;  // e.g. "Text0".."Text9" / "I" / "N" ...
};

// Parses a .tm tablemap file and returns only the p0balance..p8balance regions.
// The .tm "r$" line format is:
//   r$<name> <left> <top> <right> <bottom> <color> <radius> <transform> ...
// We only need name + the four coordinates, so this avoids the heavyweight
// CTablemap class (which is not standalone).
bool LoadBalanceRegions(const CString &tm_path, std::vector<STrainerRegion> *out);

// Rewrite the matching "r$<name> ..." line in place, replacing its colour (token 6,
// 8-hex) and radius (token 7, decimal) and keeping every other field. Returns true
// if the region line was found and the file was rewritten.
bool SaveRegionColorRadius(const CString &tm_path, const CString &name, COLORREF color, int radius);

// ---------------------------------------------------------------------------
// Thread-safe region colour cache: the single source of truth for each balance
// region's colour/radius once a tablemap is loaded. Shared by the UI thread
// (capture) and the HTTP server thread (colour re-pick). Updating it also rewrites
// the region's r$ record in the .tm so live recognition uses the same colour.
// ---------------------------------------------------------------------------
void RegionColors_Reset();
void RegionColors_SetTmPath(const CString &tm_path);
void RegionColors_Add(const CString &name, COLORREF color, int radius, const CString &transform);
bool RegionColors_GetByName(const CString &name, COLORREF *color, int *radius);
bool RegionColors_GetByTransform(const CString &transform, COLORREF *color, int *radius);
void RegionColors_UpdateByName(const CString &name, COLORREF color, int radius);   // + writes r$
