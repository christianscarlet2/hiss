#include "stdafx.h"
#include "TablemapRegions.h"
#include <map>

// True for region names p0balance .. p8balance (exactly that shape).
static bool IsBalanceRegionName(const CString &name)
{
	// p<digit>balance, digit 0..8
	if (name.GetLength() < 9) {
		return false;
	}
	if (name[0] != 'p') {
		return false;
	}
	if (name[1] < '0' || name[1] > '8') {
		return false;
	}
	CString rest = name.Mid(2);
	return rest.CompareNoCase("balance") == 0;
}

bool LoadBalanceRegions(const CString &tm_path, std::vector<STrainerRegion> *out)
{
	if (out == NULL) {
		return false;
	}
	out->clear();

	CStdioFile file;
	if (!file.Open(tm_path, CFile::modeRead | CFile::typeText)) {
		return false;
	}

	CString line;
	while (file.ReadString(line)) {
		// Region lines start with "r$".
		if (line.Left(2) != "r$") {
			continue;
		}
		// Strip the "r$" prefix, then tokenize on whitespace.
		CString body = line.Mid(2);
		int pos = 0;
		CString name = body.Tokenize(" \t", pos);
		CString sleft = body.Tokenize(" \t", pos);
		CString stop = body.Tokenize(" \t", pos);
		CString sright = body.Tokenize(" \t", pos);
		CString sbottom = body.Tokenize(" \t", pos);
		if (name.IsEmpty() || sleft.IsEmpty() || stop.IsEmpty()
			|| sright.IsEmpty() || sbottom.IsEmpty()) {
			continue;
		}
		if (!IsBalanceRegionName(name)) {
			continue;
		}
		// Optional trailing fields: colour, radius, transform.
		CString scolor = (pos >= 0) ? body.Tokenize(" \t", pos) : CString("");
		CString sradius = (pos >= 0) ? body.Tokenize(" \t", pos) : CString("");
		CString stransform = (pos >= 0) ? body.Tokenize(" \t", pos) : CString("");

		STrainerRegion region;
		region.name = name;
		region.rect.left = atol(sleft.GetString());
		region.rect.top = atol(stop.GetString());
		region.rect.right = atol(sright.GetString());
		region.rect.bottom = atol(sbottom.GetString());
		region.color = (COLORREF)strtoul(CStringA(scolor).GetString(), NULL, 16);
		region.radius = sradius.IsEmpty() ? 0 : atol(sradius.GetString());
		region.transform = stransform;
		out->push_back(region);
	}
	file.Close();
	return true;
}

bool SaveRegionColorRadius(const CString &tm_path, const CString &name, COLORREF color, int radius)
{
	if (tm_path.IsEmpty() || name.IsEmpty()) {
		return false;
	}
	// Read every line (text mode strips EOLs).
	std::vector<CStringA> lines;
	{
		CStdioFile file;
		if (!file.Open(tm_path, CFile::modeRead | CFile::typeText)) {
			return false;
		}
		CString line;
		while (file.ReadString(line)) {
			lines.push_back(CStringA(line));
		}
		file.Close();
	}

	bool found = false;
	for (size_t i = 0; i < lines.size(); ++i) {
		CString line(lines[i]);
		if (line.Left(2) != "r$") {
			continue;
		}
		// r$<name> <left> <top> <right> <bottom> <color> <radius> <transform> ...
		CString body = line.Mid(2);
		int pos = 0;
		CString rname = body.Tokenize(" \t", pos);
		if (rname.CompareNoCase(name) != 0) {
			continue;
		}
		CString sleft = body.Tokenize(" \t", pos);
		CString stop = body.Tokenize(" \t", pos);
		CString sright = body.Tokenize(" \t", pos);
		CString sbottom = body.Tokenize(" \t", pos);
		// Skip the old colour + radius tokens (positions 6 and 7) if present.
		if (pos >= 0) body.Tokenize(" \t", pos);   // old colour
		if (pos >= 0) body.Tokenize(" \t", pos);   // old radius
		// Everything still unconsumed (transform + any trailing fields) is preserved.
		CString rest = (pos >= 0) ? body.Mid(pos) : CString("");
		rest.TrimLeft();

		CString rebuilt;
		rebuilt.Format("r$%s %s %s %s %s %08x %d", rname.GetString(),
			sleft.GetString(), stop.GetString(), sright.GetString(), sbottom.GetString(),
			(unsigned int)color, radius);
		if (!rest.IsEmpty()) { rebuilt += " "; rebuilt += rest; }
		lines[i] = CStringA(rebuilt);
		found = true;
		break;
	}
	if (!found) {
		return false;
	}

	// Write back with CRLF (mirrors CTrainerFonts::SaveToTablemap).
	FILE *fp = NULL;
	if (fopen_s(&fp, CStringA(tm_path).GetString(), "wb") != 0 || fp == NULL) {
		return false;
	}
	for (size_t i = 0; i < lines.size(); ++i) {
		fwrite(lines[i].GetString(), 1, lines[i].GetLength(), fp);
		fwrite("\r\n", 1, 2, fp);
	}
	fclose(fp);
	return true;
}

// ---------------------------------------------------------------------------
// Thread-safe region colour cache
// ---------------------------------------------------------------------------
namespace {
	struct SRegionColor { COLORREF color; int radius; CString transform; };

	CRITICAL_SECTION g_rc_cs;
	bool g_rc_init = false;
	std::map<CString, SRegionColor> g_rc;   // keyed by region name
	CString g_rc_tm_path;

	void RC_EnsureInit() {
		if (!g_rc_init) { InitializeCriticalSection(&g_rc_cs); g_rc_init = true; }
	}
}

void RegionColors_Reset()
{
	RC_EnsureInit();
	EnterCriticalSection(&g_rc_cs);
	g_rc.clear();
	LeaveCriticalSection(&g_rc_cs);
}

void RegionColors_SetTmPath(const CString &tm_path)
{
	RC_EnsureInit();
	EnterCriticalSection(&g_rc_cs);
	g_rc_tm_path = tm_path;
	LeaveCriticalSection(&g_rc_cs);
}

void RegionColors_Add(const CString &name, COLORREF color, int radius, const CString &transform)
{
	RC_EnsureInit();
	EnterCriticalSection(&g_rc_cs);
	SRegionColor rc; rc.color = color; rc.radius = radius; rc.transform = transform;
	g_rc[name] = rc;
	LeaveCriticalSection(&g_rc_cs);
}

bool RegionColors_GetByName(const CString &name, COLORREF *color, int *radius)
{
	RC_EnsureInit();
	bool found = false;
	EnterCriticalSection(&g_rc_cs);
	std::map<CString, SRegionColor>::const_iterator it = g_rc.find(name);
	if (it != g_rc.end()) {
		if (color) *color = it->second.color;
		if (radius) *radius = it->second.radius;
		found = true;
	}
	LeaveCriticalSection(&g_rc_cs);
	return found;
}

bool RegionColors_GetByTransform(const CString &transform, COLORREF *color, int *radius)
{
	RC_EnsureInit();
	bool found = false;
	EnterCriticalSection(&g_rc_cs);
	for (std::map<CString, SRegionColor>::const_iterator it = g_rc.begin(); it != g_rc.end(); ++it) {
		if (it->second.transform.CompareNoCase(transform) == 0) {
			if (color) *color = it->second.color;
			if (radius) *radius = it->second.radius;
			found = true;
			break;
		}
	}
	LeaveCriticalSection(&g_rc_cs);
	return found;
}

void RegionColors_UpdateByName(const CString &name, COLORREF color, int radius)
{
	RC_EnsureInit();
	CString tm_path;
	EnterCriticalSection(&g_rc_cs);
	std::map<CString, SRegionColor>::iterator it = g_rc.find(name);
	if (it != g_rc.end()) { it->second.color = color; it->second.radius = radius; }
	tm_path = g_rc_tm_path;
	LeaveCriticalSection(&g_rc_cs);
	// File IO outside the lock.
	if (!tm_path.IsEmpty()) SaveRegionColorRadius(tm_path, name, color, radius);
}

int RegionColors_UpdateAll(COLORREF color, int radius)
{
	RC_EnsureInit();
	// Snapshot the region names + tm path under the lock, update the cache, then do
	// the file rewrites outside the lock.
	std::vector<CString> names;
	CString tm_path;
	EnterCriticalSection(&g_rc_cs);
	for (std::map<CString, SRegionColor>::iterator it = g_rc.begin(); it != g_rc.end(); ++it) {
		it->second.color = color;
		it->second.radius = radius;
		names.push_back(it->first);
	}
	tm_path = g_rc_tm_path;
	LeaveCriticalSection(&g_rc_cs);
	if (!tm_path.IsEmpty()) {
		for (size_t i = 0; i < names.size(); ++i)
			SaveRegionColorRadius(tm_path, names[i], color, radius);
	}
	return (int)names.size();
}
