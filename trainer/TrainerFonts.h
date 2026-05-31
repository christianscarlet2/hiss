#pragma once

#include <vector>
#include <map>

// Faithful port of OpenScrape/Hiss bitmap-font text recognition (the "Text0".."Text9"
// transforms) so trainer.exe can recognize scrapes via the tablemap's t$ font groups
// and create new fonts. The hashing/segmentation MUST match CTransform exactly, so the
// constants and algorithms below mirror CTransform.cpp / MagicNumbers.h verbatim.

#define TFE_MAX_CHAR_WIDTH         200   // MAX_CHAR_WIDTH
#define TFE_MAX_CHAR_HEIGHT        150   // MAX_CHAR_HEIGHT (also the character[] row stride)
#define TFE_MAX_SINGLE_CHAR_WIDTH  31    // MAX_SINGLE_CHAR_WIDTH
#define TFE_MAX_SINGLE_CHAR_HEIGHT 32    // MAX_SINGLE_CHAR_HEIGHT
#define TFE_DEFAULT_FUZZY_TOLERANCE 0.25 // DEFAULT_FUZZY_TOLERANCE
#define TFE_NUM_FONT_GROUPS        10    // k_max_number_of_font_groups_in_tablemap

// One font record (mirrors STablemapFont). Keyed in a group map by hexmash.
struct TFontRec {
	char            ch;
	int             x_count;
	unsigned int    x[TFE_MAX_SINGLE_CHAR_WIDTH];
	CStringA        hexmash;   // concatenated per-column hex (no spaces) - the lookup key
};
typedef std::map<CStringA, TFontRec> TFontGroup;

// One segmented glyph from a scrape (used by the font-creation editor).
struct TGlyph {
	CStringA        hexmash;                          // lookup key (no spaces)
	int             x_count;
	unsigned int    x[TFE_MAX_SINGLE_CHAR_WIDTH];
	char            existing_ch;                      // matched char already in the group, else 0
	std::vector<unsigned char> png;                   // PNG of the glyph mask (binarized), for display
	std::vector<unsigned char> regular_png;           // PNG of the glyph's actual (regular) pixels
	std::vector<unsigned char> full_png;              // PNG of the entire region scrape it came from
};

class CTrainerFonts {
public:
	CTrainerFonts();
	~CTrainerFonts();

	// Parse the t$ font groups + s$tXtype scan-mode symbols from a .tm file.
	bool LoadFromTablemap(const CString &tm_path);
	// Rewrite the t$ section of the loaded .tm with the current in-memory groups.
	bool SaveToTablemap();
	CString tm_path() const { return _tm_path; }

	// Recognize a top-down 32bpp BGRA crop using font group g and the region's
	// colour cube. Returns the recognized text (empty if nothing matched).
	CString RecognizeBgra(const unsigned char *bgra, int w, int h,
		COLORREF color, int radius, int group);

	// Segment a BGRA crop into individual glyphs (for the font editor). Each glyph
	// gets its hexmash/x[]/x_count, any already-known character, and a display PNG.
	void SegmentBgra(const unsigned char *bgra, int w, int h,
		COLORREF color, int radius, int group, std::vector<TGlyph> *out);

	// Render the foreground colour-cube mask of a BGRA crop to a PNG (the
	// "transformed" view for font-hash recognition). Scaled up for visibility.
	void MaskPng(const unsigned char *bgra, int w, int h, COLORREF color, int radius,
		std::vector<unsigned char> *png);

	// Insert/replace a glyph's font record in group g with character ch. Thread-safe.
	void SetGlyph(int group, const TGlyph &glyph, char ch);
	// Remove a hexmash from a group (thread-safe).
	void RemoveHexmash(int group, const CStringA &hexmash);

	int      group_count(int g);     // number of records currently in group g
	CStringA scan_mode(int g);       // "plain" / "fuzzy" / numeric tolerance ("plain" default)

private:
	CRITICAL_SECTION _cs;
	CString          _tm_path;
	TFontGroup       _groups[TFE_NUM_FONT_GROUPS];
	CStringA         _scanmode[TFE_NUM_FONT_GROUPS];
};

extern CTrainerFonts *p_trainer_fonts;
