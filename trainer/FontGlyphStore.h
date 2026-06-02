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
	COLORREF  color;        // colour cube centre used to segment this glyph (editable)
	int       radius;       // colour cube radius (editable)
	std::vector<unsigned char> src_bgra;   // the source region crop (top-down 32bpp BGRA)
	int       src_w, src_h;                // its dimensions, for re-segmentation
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
	// `color`/`radius` are the colour cube it was segmented with; `src_bgra`/w/h is
	// the source region crop kept for later re-segmentation (colour re-pick).
	int Add(const CStringA &region, int group, const TGlyph &glyph,
		COLORREF color, int radius,
		const std::vector<unsigned char> &src_bgra, int src_w, int src_h);

	CStringA ListJson();                                  // [{gid,region,group,hexmash,assigned,a,r,g,b,radius}]
	bool GetImage(int gid, std::vector<unsigned char> *out);          // mask PNG
	bool GetRegularImage(int gid, std::vector<unsigned char> *out);   // glyph actual-pixels PNG
	bool GetFullImage(int gid, std::vector<unsigned char> *out);      // entire region scrape PNG

	// Copy the glyph's REFERENCE sub-crop ([xb..xe,yb..ye] of the source region, in
	// original colours) as a top-down 32bpp BGRA buffer, plus its colour/radius/group
	// and source region name. Used to OCR a single glyph for the CHAR autopopulate.
	bool GetReferenceBgra(int gid, std::vector<unsigned char> *out, int *w, int *h,
		COLORREF *color, int *radius, int *group, CStringA *region);

	// Read the source ARGB at a natural PNG pixel of the glyph's "regular" (glyph
	// sub-crop, 6x) or "full" (whole region, 3x) image. Returns false if out of range.
	bool GetPixel(int gid, const CStringA &img, int px, int py,
		int *a, int *r, int *g, int *b);

	// Re-segment this glyph with a new colour/radius (keeps the row, matched by the
	// glyph's original x-centre) and persist the colour/radius to the region's r$.
	bool Regen(int gid, int a, int r, int g, int b, int radius);

	// Set the save group, pull the colour/radius default from a region whose transform
	// is "Text<group>", regenerate, and persist. Outputs the applied colour/radius.
	bool SetGroupDefaults(int gid, int group, int *a, int *r, int *g, int *b, int *radius);

	// Apply group + colour + radius to every row strictly below `gid`, regenerating
	// each from its own source crop and persisting r$. Returns the number affected.
	int ApplyBelow(int gid, int group, int a, int r, int g, int b, int radius);

	// Apply colour + radius to every pending glyph currently assigned to `group`
	// (the global per-group picker), regenerating each and persisting r$. Returns count.
	int ApplyToGroup(int group, int a, int r, int g, int b, int radius);

	// Apply colour + radius to EVERY balance region's r$ (so a re-capture binarises
	// with it, even for regions that currently have no pending glyphs) and regenerate
	// every pending glyph. Returns the number of pending glyphs regenerated; outputs
	// the number of regions updated via *regions_out.
	int ApplyToAll(int a, int r, int g, int b, int radius, int *regions_out);

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

	// Undo the last removal: restores the row(s) that were deleted, or — for a row
	// that was labeled (font created) — removes that font from the tablemap, re-saves,
	// and restores the row. Returns the number of rows restored (0 = nothing to undo).
	int Undo();

	// Save every labeled glyph into its font group, then persist the tablemap.
	// Returns the number of glyph records written.
	int SaveAll();

	// The font group the editor is currently capturing/working in.
	void SetEditGroup(int group);
	int  EditGroup();

private:
	// One reversible removal. type 0 = plain delete; type 1 = label/create (the
	// font in `group`/`hexmash` was written to the tablemap and must be removed on undo).
	struct SUndoAction {
		int type;
		int group;
		CStringA hexmash;
		std::vector<SFontGlyphEntry> entries;   // row(s) to restore
	};

	CRITICAL_SECTION _cs;
	std::vector<SFontGlyphEntry> _entries;
	std::vector<SUndoAction> _undo;
	int _next_gid;
	int _edit_group;
};

extern CFontGlyphStore *p_font_glyph_store;
