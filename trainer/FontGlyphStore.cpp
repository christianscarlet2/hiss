#include "stdafx.h"
#include "FontGlyphStore.h"
#include "TablemapRegions.h"

CFontGlyphStore *p_font_glyph_store = NULL;

CFontGlyphStore::CFontGlyphStore()
{
	InitializeCriticalSection(&_cs);
	_next_gid = 1;
	_edit_group = 0;
}

CFontGlyphStore::~CFontGlyphStore()
{
	DeleteCriticalSection(&_cs);
}

int CFontGlyphStore::Add(const CStringA &region, int group, const TGlyph &glyph,
	COLORREF color, int radius,
	const std::vector<unsigned char> &src_bgra, int src_w, int src_h)
{
	if (group < 0 || group >= TFE_NUM_FONT_GROUPS || glyph.x_count <= 0) return -1;
	EnterCriticalSection(&_cs);
	// Skip glyphs already known in the font group (only unknown ones need labels).
	if (glyph.existing_ch != 0) { LeaveCriticalSection(&_cs); return -1; }
	// Skip duplicates already pending for the same group.
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].group == group && _entries[i].glyph.hexmash == glyph.hexmash) {
			LeaveCriticalSection(&_cs);
			return -1;
		}
	}
	SFontGlyphEntry e;
	e.gid = _next_gid++;
	e.region = region;
	e.group = group;
	e.glyph = glyph;
	e.assigned = "";
	e.color = color;
	e.radius = radius;
	e.src_bgra = src_bgra;
	e.src_w = src_w;
	e.src_h = src_h;
	_entries.push_back(e);
	int gid = e.gid;
	LeaveCriticalSection(&_cs);
	return gid;
}

static CStringA JsonEscapeA(const CStringA &value)
{
	CStringA out;
	for (int i = 0; i < value.GetLength(); ++i) {
		unsigned char c = (unsigned char)value[i];
		switch (c) {
		case '\\': out += "\\\\"; break;
		case '"': out += "\\\""; break;
		case '\r': break;
		case '\n': out += "\\n"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) { char b[8]; sprintf_s(b, sizeof(b), "\\u%04x", c); out += b; }
			else out += (char)c;
			break;
		}
	}
	return out;
}

CStringA CFontGlyphStore::ListJson()
{
	CStringA json = "[";
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _entries.size(); ++i) {
		const SFontGlyphEntry &e = _entries[i];
		if (i > 0) json += ",";
		CStringA entry;
		entry.Format("{\"gid\":%d,\"region\":\"%s\",\"group\":%d,\"hexmash\":\"%s\",\"assigned\":\"%s\","
			"\"a\":%d,\"r\":%d,\"g\":%d,\"b\":%d,\"radius\":%d}",
			e.gid,
			JsonEscapeA(e.region).GetString(),
			e.group,
			JsonEscapeA(e.glyph.hexmash).GetString(),
			JsonEscapeA(e.assigned).GetString(),
			(int)((e.color >> 24) & 0xff),
			(int)GetRValue(e.color),
			(int)GetGValue(e.color),
			(int)GetBValue(e.color),
			e.radius);
		json += entry;
	}
	LeaveCriticalSection(&_cs);
	json += "]";
	return json;
}

bool CFontGlyphStore::GetImage(int gid, std::vector<unsigned char> *out)
{
	bool found = false;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].gid == gid) { *out = _entries[i].glyph.png; found = true; break; }
	}
	LeaveCriticalSection(&_cs);
	return found;
}

bool CFontGlyphStore::GetRegularImage(int gid, std::vector<unsigned char> *out)
{
	bool found = false;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].gid == gid) { *out = _entries[i].glyph.regular_png; found = true; break; }
	}
	LeaveCriticalSection(&_cs);
	return found;
}

bool CFontGlyphStore::GetFullImage(int gid, std::vector<unsigned char> *out)
{
	bool found = false;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].gid == gid) { *out = _entries[i].glyph.full_png; found = true; break; }
	}
	LeaveCriticalSection(&_cs);
	return found;
}

// Assemble a COLORREF from A/R/G/B the way the colour cube reads it back
// (low byte = R via GetRValue, alpha in the high byte).
static COLORREF ArgbToColorref(int a, int r, int g, int b)
{
	return RGB(r & 0xff, g & 0xff, b & 0xff) | ((COLORREF)(a & 0xff) << 24);
}

bool CFontGlyphStore::GetPixel(int gid, const CStringA &img, int px, int py,
	int *a, int *r, int *g, int *b)
{
	bool ok = false;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].gid != gid) continue;
		const SFontGlyphEntry &e = _entries[i];
		// Map the natural PNG pixel back to a source-region pixel.
		// "regular" = glyph sub-crop [xb..xe,yb..ye] scaled 6x; "full" = whole region scaled 3x.
		int rx, ry;
		if (img == "regular") { rx = e.glyph.xb + px / 6; ry = e.glyph.yb + py / 6; }
		else                  { rx = px / 3;              ry = py / 3; }
		if (rx >= 0 && ry >= 0 && rx < e.src_w && ry < e.src_h
			&& (size_t)((ry * e.src_w + rx) * 4 + 3) < e.src_bgra.size()) {
			int base = (ry * e.src_w + rx) * 4;
			if (b) *b = e.src_bgra[base + 0];
			if (g) *g = e.src_bgra[base + 1];
			if (r) *r = e.src_bgra[base + 2];
			if (a) *a = e.src_bgra[base + 3];
			ok = true;
		}
		break;
	}
	LeaveCriticalSection(&_cs);
	return ok;
}

bool CFontGlyphStore::Regen(int gid, int a, int r, int g, int b, int radius)
{
	bool ok = false;
	CStringA region; COLORREF color = 0;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].gid != gid) continue;
		SFontGlyphEntry &e = _entries[i];
		color = ArgbToColorref(a, r, g, b);
		e.color = color;
		e.radius = radius;
		if (p_trainer_fonts != NULL && !e.src_bgra.empty()) {
			int center_x = (e.glyph.xb + e.glyph.xe) / 2;
			TGlyph ng;
			p_trainer_fonts->RegenGlyphAt(&e.src_bgra[0], e.src_w, e.src_h,
				color, radius, e.group, center_x, &ng);
			e.glyph = ng;   // replace mask/hexmash/bounds (kept even if empty)
		}
		region = e.region;
		ok = true;
		break;
	}
	LeaveCriticalSection(&_cs);
	// Persist the colour/radius to this region's r$ record (file IO outside the lock).
	if (ok && !region.IsEmpty()) RegionColors_UpdateByName(CString(region), color, radius);
	return ok;
}

bool CFontGlyphStore::SetGroupDefaults(int gid, int group, int *a, int *r, int *g, int *b, int *radius)
{
	if (group < 0 || group >= TFE_NUM_FONT_GROUPS) return false;
	// Look up the colour/radius default for this transform (Text<group>) outside the lock.
	CString transform; transform.Format("Text%d", group);
	COLORREF defc = 0; int defr = 0;
	bool have_default = RegionColors_GetByTransform(transform, &defc, &defr);

	bool ok = false;
	CStringA region; COLORREF color = 0; int rad = 0;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].gid != gid) continue;
		SFontGlyphEntry &e = _entries[i];
		e.group = group;
		if (have_default) { e.color = defc; e.radius = defr; }
		color = e.color; rad = e.radius;
		if (p_trainer_fonts != NULL && !e.src_bgra.empty()) {
			int center_x = (e.glyph.xb + e.glyph.xe) / 2;
			TGlyph ng;
			p_trainer_fonts->RegenGlyphAt(&e.src_bgra[0], e.src_w, e.src_h,
				color, rad, e.group, center_x, &ng);
			e.glyph = ng;
		}
		region = e.region;
		ok = true;
		break;
	}
	_edit_group = group;
	LeaveCriticalSection(&_cs);
	if (ok) {
		if (a) *a = (int)((color >> 24) & 0xff);
		if (r) *r = (int)GetRValue(color);
		if (g) *g = (int)GetGValue(color);
		if (b) *b = (int)GetBValue(color);
		if (radius) *radius = rad;
		if (!region.IsEmpty()) RegionColors_UpdateByName(CString(region), color, rad);
	}
	return ok;
}

int CFontGlyphStore::ApplyBelow(int gid, int group, int a, int r, int g, int b, int radius)
{
	if (group < 0 || group >= TFE_NUM_FONT_GROUPS) return 0;
	COLORREF color = ArgbToColorref(a, r, g, b);
	std::vector<CStringA> touched;   // regions to persist (outside the lock)
	int count = 0;
	EnterCriticalSection(&_cs);
	// Find the anchor row, then apply to every entry strictly after it.
	int anchor = -1;
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].gid == gid) { anchor = (int)i; break; }
	}
	if (anchor >= 0) {
		for (size_t i = anchor + 1; i < _entries.size(); ++i) {
			SFontGlyphEntry &e = _entries[i];
			e.group = group;
			e.color = color;
			e.radius = radius;
			if (p_trainer_fonts != NULL && !e.src_bgra.empty()) {
				int center_x = (e.glyph.xb + e.glyph.xe) / 2;
				TGlyph ng;
				p_trainer_fonts->RegenGlyphAt(&e.src_bgra[0], e.src_w, e.src_h,
					color, radius, e.group, center_x, &ng);
				e.glyph = ng;
			}
			touched.push_back(e.region);
			++count;
		}
	}
	LeaveCriticalSection(&_cs);
	for (size_t i = 0; i < touched.size(); ++i) {
		if (!touched[i].IsEmpty()) RegionColors_UpdateByName(CString(touched[i]), color, radius);
	}
	return count;
}

bool CFontGlyphStore::SetGroupFor(int gid, int group)
{
	if (group < 0 || group >= TFE_NUM_FONT_GROUPS) return false;
	bool found = false;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].gid == gid) { _entries[i].group = group; found = true; break; }
	}
	_edit_group = group;   // sticky default for the next capture
	LeaveCriticalSection(&_cs);
	return found;
}

int CFontGlyphStore::AssignChar(int gid, const CStringA &ch)
{
	int removed = 0;
	EnterCriticalSection(&_cs);

	CStringA v = ch;
	if (v.GetLength() > 1) v = v.Left(1);   // a glyph maps to a single character

	int create_group = -1;
	TGlyph create_glyph;
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].gid == gid) {
			_entries[i].assigned = v;
			if (!v.IsEmpty()) { create_group = _entries[i].group; create_glyph = _entries[i].glyph; }
			break;
		}
	}

	// Create the font now (in memory for instant recognition + persisted), then
	// drop every pending glyph whose font now exists (this one + any duplicates).
	if (!v.IsEmpty() && create_group >= 0 && p_trainer_fonts != NULL) {
		p_trainer_fonts->SetGlyph(create_group, create_glyph, v[0]);
		p_trainer_fonts->SaveToTablemap();
		SUndoAction act;
		act.type = 1;
		act.group = create_group;
		act.hexmash = create_glyph.hexmash;
		for (size_t i = _entries.size(); i-- > 0; ) {
			if (p_trainer_fonts->HasHexmash(_entries[i].group, _entries[i].glyph.hexmash)) {
				act.entries.push_back(_entries[i]);
				_entries.erase(_entries.begin() + i);
				++removed;
			}
		}
		_undo.push_back(act);
		if (_undo.size() > 100) _undo.erase(_undo.begin());
	}

	LeaveCriticalSection(&_cs);
	return removed;
}

bool CFontGlyphStore::Delete(int gid)
{
	bool found = false;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].gid == gid) {
			SUndoAction act;
			act.type = 0;
			act.group = _entries[i].group;
			act.hexmash = _entries[i].glyph.hexmash;
			act.entries.push_back(_entries[i]);
			_undo.push_back(act);
			if (_undo.size() > 100) _undo.erase(_undo.begin());
			_entries.erase(_entries.begin() + i);
			found = true;
			break;
		}
	}
	LeaveCriticalSection(&_cs);
	return found;
}

void CFontGlyphStore::Clear()
{
	EnterCriticalSection(&_cs);
	_entries.clear();
	_undo.clear();
	LeaveCriticalSection(&_cs);
}

int CFontGlyphStore::Undo()
{
	int restored = 0;
	EnterCriticalSection(&_cs);
	if (!_undo.empty()) {
		SUndoAction act = _undo.back();
		_undo.pop_back();
		// A labeled/created row: remove its font from the tablemap and re-save.
		if (act.type == 1 && p_trainer_fonts != NULL) {
			p_trainer_fonts->RemoveHexmash(act.group, act.hexmash);
			p_trainer_fonts->SaveToTablemap();
		}
		// Restore the row(s) as fresh, unlabeled glyphs.
		for (size_t i = 0; i < act.entries.size(); ++i) {
			SFontGlyphEntry e = act.entries[i];
			e.gid = _next_gid++;
			e.assigned = "";
			_entries.push_back(e);
			++restored;
		}
	}
	LeaveCriticalSection(&_cs);
	return restored;
}

int CFontGlyphStore::SaveAll()
{
	int written = 0;
	EnterCriticalSection(&_cs);
	if (p_trainer_fonts != NULL) {
		for (size_t i = 0; i < _entries.size(); ++i) {
			SFontGlyphEntry &e = _entries[i];
			if (e.assigned.IsEmpty()) continue;
			p_trainer_fonts->SetGlyph(e.group, e.glyph, e.assigned[0]);
			++written;
		}
		if (written > 0) {
			p_trainer_fonts->SaveToTablemap();
			// Drop the entries we just persisted so they don't reappear.
			for (size_t i = _entries.size(); i-- > 0; )
				if (!_entries[i].assigned.IsEmpty())
					_entries.erase(_entries.begin() + i);
		}
	}
	LeaveCriticalSection(&_cs);
	return written;
}

void CFontGlyphStore::SetEditGroup(int group)
{
	EnterCriticalSection(&_cs);
	if (group >= 0 && group < TFE_NUM_FONT_GROUPS) _edit_group = group;
	LeaveCriticalSection(&_cs);
}

int CFontGlyphStore::EditGroup()
{
	EnterCriticalSection(&_cs);
	int g = _edit_group;
	LeaveCriticalSection(&_cs);
	return g;
}
