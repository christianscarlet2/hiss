#include "stdafx.h"
#include "FontGlyphStore.h"

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

int CFontGlyphStore::Add(const CStringA &region, int group, const TGlyph &glyph)
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
		entry.Format("{\"gid\":%d,\"region\":\"%s\",\"group\":%d,\"hexmash\":\"%s\",\"assigned\":\"%s\"}",
			e.gid,
			JsonEscapeA(e.region).GetString(),
			e.group,
			JsonEscapeA(e.glyph.hexmash).GetString(),
			JsonEscapeA(e.assigned).GetString());
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

bool CFontGlyphStore::SetChar(int gid, const CStringA &ch)
{
	bool found = false;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].gid == gid) {
			CStringA v = ch;
			if (v.GetLength() > 1) v = v.Left(1);   // a glyph maps to a single character
			_entries[i].assigned = v;
			found = true;
			break;
		}
	}
	LeaveCriticalSection(&_cs);
	return found;
}

bool CFontGlyphStore::Delete(int gid)
{
	bool found = false;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < _entries.size(); ++i) {
		if (_entries[i].gid == gid) { _entries.erase(_entries.begin() + i); found = true; break; }
	}
	LeaveCriticalSection(&_cs);
	return found;
}

void CFontGlyphStore::Clear()
{
	EnterCriticalSection(&_cs);
	_entries.clear();
	LeaveCriticalSection(&_cs);
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
