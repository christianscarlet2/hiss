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
