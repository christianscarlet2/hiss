#pragma once

#include <vector>

// A single tablemap region we care about (player balance / name fields).
struct STrainerRegion {
	CString  name;
	RECT     rect;       // left/top/right/bottom in client pixels
	COLORREF color;      // ARGB colour cube centre (alpha in high byte)
	int      radius;     // colour cube radius
	CString  transform;  // e.g. "Text0".."Text9" / "I" / "N" ...
	CString  field_type; // "balance" or "name" (the pN<type> suffix), lowercased
	bool     enabled;    // is this field type in the shared scrape_fields list?
	// Auto Cropper (shared with Vision/Hiss via tm_regions). When ac_enabled and a
	// colour is on, the scrape is cropped to the bounding box of pixels matching any
	// enabled colour cube. Each colour packs ARGB; alpha ignored for matching.
	bool     ac_enabled = false;
	COLORREF ac_color1 = 0; int ac_tol1 = 0; bool ac_c1en = false;
	COLORREF ac_color2 = 0; int ac_tol2 = 0; bool ac_c2en = false;
	COLORREF ac_color3 = 0; int ac_tol3 = 0; bool ac_c3en = false;
};

// The set of player field types the trainer scrapes (p0balance..p8balance,
// p0name..p8name). Read once from the shared `scrape_fields` setting; defaults to
// just "balance" when unset. Each loaded region's `enabled` flag is set from this.

// Parses a .tm tablemap file and returns the p0balance..p8balance / p0name..p8name regions.
// The .tm "r$" line format is:
//   r$<name> <left> <top> <right> <bottom> <color> <radius> <transform> ...
// We only need name + the four coordinates, so this avoids the heavyweight
// CTablemap class (which is not standalone).
bool LoadBalanceRegions(const CString &tm_path, std::vector<STrainerRegion> *out);

// Same as LoadBalanceRegions but reads from the PostgreSQL "hiss" database.
// tm_name is the tablemap's name (DB key, e.g. the former .tm filename stem).
bool LoadBalanceRegionsFromDB(const CString &tm_name, std::vector<STrainerRegion> *out);

// Re-read the shared `scrape_fields` list and update each region's `enabled` flag in
// place (cheap; no DB region reload). Call after the list changes.
void TrainerRegions_RefreshEnabled(std::vector<STrainerRegion> *regions);

// True if `region_name` matches a field type in the shared `decimal_split_fields` list
// (same logic Hiss uses), so the trainer's preview/capture decimal-split decision
// respects that list instead of a global toggle.
bool TrainerRegionUsesDecimalSplit(const CString &region_name);

// Persist a region's transform method (e.g. "A0"/"A1") to tm_regions in the database.
// tm_name is the tablemap NAME (DB key). Returns true on a successful write.
bool SaveRegionTransform(const CString &tm_name, const CString &name, const CString &transform);

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
int  RegionColors_UpdateAll(COLORREF color, int radius);   // set every region; returns count, writes r$
