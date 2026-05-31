#pragma once

#include <vector>
#include "TrainerFonts.h"

// One pending glyph in the font-creation editor: a segmented glyph from some
// scrape, the font group it belongs to, and the character the user assigns.
struct SFontGlyphEntry {
	int       gid;
	CStringA  region;       // which scrape region it came from
	int       group;        // Text0..Text9 group it will be saved into
	TGlyph    glyph;        // hexmash / x[] / x_count / display png
	CStringA  assigned;     // character the user typed (empty until labeled)
};

// Thread-safe store shared by the capture (UI) thread and the HTTP server thread.
// Glyphs from multiple scrapes accumulate here; the user labels them and saves all
// at once into the tablemap's t$ font groups.
class CFontGlyphStore {
public:
	CFontGlyphStore();
	~CFontGlyphStore();

	// Add a freshly-segmented glyph (skips ones already pending for the same group,
	// or already present in the font group). Returns the new gid, or -1 if skipped.
	int Add(const CStringA &region, int group, const TGlyph &glyph);

	CStringA ListJson();                                  // [{gid,region,group,hexmash,assigned,known}]
	bool GetImage(int gid, std::vector<unsigned char> *out);          // mask PNG
	bool GetRegularImage(int gid, std::vector<unsigned char> *out);   // glyph actual-pixels PNG
	bool GetFullImage(int gid, std::vector<unsigned char> *out);      // entire region scrape PNG

	// Set which font group this glyph saves to (also becomes the sticky default
	// for subsequent captures).
	bool SetGroupFor(int gid, int group);

	// Assign a character to a glyph. A non-empty single character creates the font
	// immediately in the glyph's group (in memory + persisted to the .tm), then
	// drops every pending glyph whose font now exists (the new one + duplicates).
	// Returns the number of pending glyphs removed.
	int AssignChar(int gid, const CStringA &ch);

	bool Delete(int gid);
	void Clear();

	// Save every labeled glyph into its font group, then persist the tablemap.
	// Returns the number of glyph records written.
	int SaveAll();

	// The font group the editor is currently capturing/working in.
	void SetEditGroup(int group);
	int  EditGroup();

private:
	CRITICAL_SECTION _cs;
	std::vector<SFontGlyphEntry> _entries;
	int _next_gid;
	int _edit_group;
};

extern CFontGlyphStore *p_font_glyph_store;
