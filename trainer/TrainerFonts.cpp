#include "stdafx.h"
#include "TrainerFonts.h"
#include "TrainerDB.h"
#include "libpq-fe.h"

#include <math.h>
#include <stdio.h>
#include <opencv2/opencv.hpp>

CTrainerFonts *p_trainer_fonts = NULL;

// ===========================================================================
// Ported helpers - byte-for-byte equivalents of CTransform.cpp so the hashes
// computed here match what OpenScrape/Hiss compute. The 2D character array is
// stored flat with a caller-supplied row stride (chStride): cell (x,y) lives at
// ch[x * chStride + y]. background[] is one byte per column.
// ===========================================================================

static int TFE_bitcount(unsigned int n)
{
	int c = 0;
	while (n) { ++c; n &= n - 1; }
	return c;
}

static int TFE_CalcHammingDistance(unsigned int x, unsigned int y)
{
	int dist = 0, val = x ^ y;
	while (val) { ++dist; val &= val - 1; }
	return dist;
}

static bool TFE_IsInARGBColorCube(int center_a, int center_r, int center_g, int center_b,
	int radius, int pix_a, int pix_r, int pix_g, int pix_b)
{
	int a_diff = 0, r_diff = 0, g_diff = 0, b_diff = 0, tot_diff = 0;
	if (radius == 0) {
		if (pix_a == center_a && pix_r == center_r && pix_g == center_g && pix_b == center_b)
			return true;
	} else {
		a_diff = center_a - pix_a;
		r_diff = center_r - pix_r;
		g_diff = center_g - pix_g;
		b_diff = center_b - pix_b;
		tot_diff = (int)pow((double)(a_diff*a_diff + r_diff*r_diff + g_diff*g_diff + b_diff*b_diff), 0.5);
		if (radius >= 0) {
			if (tot_diff <= radius) return true;
		} else {
			if (tot_diff > -radius) return true;
		}
	}
	return false;
}

static void TFE_GetShiftLeftDownIndexes(int x_start, int width, int height,
	const unsigned char *bg, const unsigned char *ch, int chStride,
	int *x_begin, int *x_end, int *y_begin, int *y_end)
{
	int x = 0, y = 0;
	*x_begin = x_start + width - 1;
	*x_end = x_start;
	for (x = x_start; x < x_start + width; x++) {
		if (!bg[x]) { *x_begin = x; x = x_start + width + 1; }
	}
	for (x = x_start + width - 1; x >= x_start; x--) {
		if (!bg[x]) { *x_end = x; x = -1; }
	}
	*y_begin = height - 1;
	*y_end = 0;
	for (y = 0; y < height; y++) {
		for (x = *x_begin; x <= *x_end; x++) {
			if (ch[x * chStride + y]) { *y_begin = y; x = *x_end + 1; y = height + 1; }
		}
	}
	for (y = height - 1; y >= 0; y--) {
		for (x = *x_begin; x <= *x_end; x++) {
			if (ch[x * chStride + y]) { *y_end = y; x = *x_end + 1; y = -1; }
		}
	}
}

static void TFE_CalcHexmash(int left, int right, int top, int bottom,
	const unsigned char *ch, int chStride, CStringA *hexmash, bool withspace)
{
	int x = 0, y = 0, last_fg_row = -1;
	unsigned int hexval = 0;
	char t[20] = { 0 };

	for (y = bottom; y >= top; y--) {
		for (x = left; x <= right; x++) {
			if (ch[x * chStride + y]) { last_fg_row = y; x = right + 1; y = -1; }
		}
	}

	*hexmash = "";
	for (x = left; x <= right; x++) {
		hexval = 0;
		for (y = last_fg_row; y >= top; y--)
			if (ch[x * chStride + y])
				hexval += (1 << (last_fg_row - y));
		sprintf_s(t, 20, "%x", hexval);
		*hexmash += t;
		if (withspace) *hexmash += " ";
	}
}

// Build the character[]/background[] arrays from a top-down 32bpp BGRA crop,
// using the region colour cube (mirrors TTypeTransform's pixel loop).
// chStride is returned so callers index ch[x*chStride + y].
static void TFE_BuildCharArray(const unsigned char *bgra, int width, int height,
	COLORREF color, int radius, std::vector<unsigned char> *ch, std::vector<unsigned char> *bg,
	int *chStride)
{
	int stride = height + 2;            // pad so the "<=height" scans read a zero cell
	*chStride = stride;
	ch->assign((size_t)width * stride, 0);
	bg->assign((size_t)width, 1);       // all-background until a foreground pixel is seen

	int center_a = (color >> 24) & 0xff;
	int center_r = GetRValue(color);
	int center_g = GetGValue(color);
	int center_b = GetBValue(color);

	for (int x = 0; x < width; x++) {
		for (int y = 0; y < height; y++) {
			int base = (y * width + x) * 4;
			int blue  = bgra[base + 0];
			int green = bgra[base + 1];
			int red   = bgra[base + 2];
			int alpha = bgra[base + 3];
			if (TFE_IsInARGBColorCube(center_a, center_r, center_g, center_b, radius,
				alpha, red, green, blue)) {
				(*ch)[x * stride + y] = 1;
				(*bg)[x] = 0;
			}
		}
	}
}

// Plain (exact-hash) scan - mirrors DoPlainFontScan.
static CString TFE_PlainScan(const TFontGroup &group, int width, int height,
	const unsigned char *bg, const unsigned char *ch, int chStride)
{
	CString text = "";
	int x_begin = 0, x_end = 0, y_begin = 0, y_end = 0;
	int temp_right = 0, vert_band_left = 0;
	CStringA hexmash;

	bool backg_band = true;
	while (vert_band_left < width && backg_band) {
		for (int y = 0; y <= height; y++)
			if (ch[vert_band_left * chStride + y]) { backg_band = false; y = height + 1; }
		if (backg_band) vert_band_left++;
	}

	while (vert_band_left < width) {
		TFE_GetShiftLeftDownIndexes(vert_band_left, TFE_MAX_SINGLE_CHAR_WIDTH, height, bg, ch, chStride,
			&x_begin, &x_end, &y_begin, &y_end);
		if (y_end - y_begin > TFE_MAX_SINGLE_CHAR_HEIGHT)
			y_begin = y_end - TFE_MAX_SINGLE_CHAR_HEIGHT;

		temp_right = vert_band_left + TFE_MAX_SINGLE_CHAR_WIDTH;
		if (temp_right > width) temp_right = width;

		bool continue_looping = true;
		do {
			TFE_CalcHexmash(vert_band_left, temp_right, y_begin, y_end, ch, chStride, &hexmash, false);
			TFontGroup::const_iterator it = group.find(hexmash);
			if (it != group.end()) {
				CString nc; nc.Format("%c", it->second.ch);
				text.Append(nc);
				vert_band_left = temp_right + 1;
				continue_looping = false;
			} else {
				if (temp_right > vert_band_left) temp_right--;
				else { vert_band_left++; continue_looping = false; }
			}
		} while (continue_looping);
	}
	return text;
}

// Fuzzy (Hamming) scan - mirrors DoFuzzyFontScan + GetBestHammingDistance.
static const TFontRec *TFE_BestHamming(const TFontGroup &group, int width, int height,
	const unsigned char *bg, const unsigned char *ch, int chStride, int left, double tolerance)
{
	double best_weighted_hd = 999999.0;
	unsigned int hexval_array[TFE_MAX_SINGLE_CHAR_WIDTH] = { 0 };
	const TFontRec *best = NULL;

	for (int x = 0; x < TFE_MAX_SINGLE_CHAR_WIDTH && left + x < width; x++) {
		int x_begin = 0, x_end = 0, y_begin = 0, y_end = 0;
		TFE_GetShiftLeftDownIndexes(left, x + 1, height, bg, ch, chStride, &x_begin, &x_end, &y_begin, &y_end);
		if (y_end - y_begin > TFE_MAX_SINGLE_CHAR_HEIGHT)
			y_begin = y_end - TFE_MAX_SINGLE_CHAR_HEIGHT;

		for (int i = 0; i < TFE_MAX_SINGLE_CHAR_WIDTH; i++) hexval_array[i] = 0;
		for (int i = x_begin; i <= x_end; i++)
			for (int y = y_end; y >= y_begin; y--)
				if (ch[i * chStride + y])
					hexval_array[i - x_begin] += (1 << (y_end - y));

		for (TFontGroup::const_iterator it = group.begin(); it != group.end(); ++it) {
			double lit_pixels = 0.000001;
			double tot_hd = 0.000001;
			double weighted_hd = 999999.0;
			if (it->second.x_count <= x + 1) {
				for (int j = 0; j < it->second.x_count && j < x + 1; j++) {
					tot_hd += TFE_CalcHammingDistance(it->second.x[j], hexval_array[j]);
					lit_pixels += TFE_bitcount(it->second.x[j]);
				}
				weighted_hd = tot_hd / lit_pixels;
			}
			if (weighted_hd < tolerance && weighted_hd < best_weighted_hd) {
				best = &it->second;
				best_weighted_hd = weighted_hd;
				if (tot_hd > lit_pixels) break;
			}
		}
	}
	return best;
}

static CString TFE_FuzzyScan(const TFontGroup &group, int width, int height,
	const unsigned char *bg, const unsigned char *ch, int chStride, double tolerance)
{
	CString text = "";
	int vert_band_left = 0;
	while (vert_band_left < width) {
		bool backg_band = true;
		while (vert_band_left < width && backg_band) {
			for (int y = 0; y <= height; y++)
				if (ch[vert_band_left * chStride + y]) { backg_band = false; y = height + 1; }
			if (backg_band) vert_band_left++;
		}
		const TFontRec *match = TFE_BestHamming(group, width, height, bg, ch, chStride, vert_band_left, tolerance);
		if (match != NULL) {
			CString nc; nc.Format("%c", match->ch);
			text.Append(nc);
			vert_band_left = vert_band_left + match->x_count;
		} else {
			vert_band_left += 1;
		}
	}
	return text;
}

// Render a glyph (from its x[] columns) to a scaled PNG for display.
static void TFE_GlyphToPng(const TGlyph &g, std::vector<unsigned char> *png)
{
	png->clear();
	if (g.x_count <= 0) return;
	unsigned int allbits = 0;
	for (int j = 0; j < g.x_count; j++) allbits |= g.x[j];
	int maxbit = 0;
	for (int b = 0; b < 32; b++) if (allbits & (1u << b)) maxbit = b;
	int gh = maxbit + 1;
	if (gh < 1) gh = 1;
	cv::Mat mask(gh, g.x_count, CV_8UC1, cv::Scalar(0));
	for (int j = 0; j < g.x_count; j++)
		for (int r = 0; r < gh; r++)
			if (g.x[j] & (1u << (maxbit - r)))
				mask.at<unsigned char>(r, j) = 255;
	int scale = 6;
	cv::Mat big;
	cv::resize(mask, big, cv::Size(g.x_count * scale, gh * scale), 0, 0, cv::INTER_NEAREST);
	cv::Mat bgr;
	cv::cvtColor(big, bgr, cv::COLOR_GRAY2BGR);
	std::vector<int> params;
	params.push_back(cv::IMWRITE_PNG_COMPRESSION);
	params.push_back(3);
	cv::imencode(".png", bgr, *png, params);
}

// Crop the glyph's actual (regular) pixels from the BGRA buffer and encode a
// scaled-up PNG for side-by-side reference next to the binarized mask.
static void TFE_RegularGlyphPng(const unsigned char *bgra, int w, int h,
	int xb, int xe, int yb, int ye, std::vector<unsigned char> *png)
{
	png->clear();
	if (xb < 0) xb = 0; if (yb < 0) yb = 0;
	if (xe >= w) xe = w - 1; if (ye >= h) ye = h - 1;
	int gw = xe - xb + 1, gh = ye - yb + 1;
	if (gw < 1 || gh < 1) return;
	cv::Mat crop(gh, gw, CV_8UC3);
	for (int y = 0; y < gh; y++) {
		for (int x = 0; x < gw; x++) {
			int base = ((yb + y) * w + (xb + x)) * 4;
			crop.at<cv::Vec3b>(y, x) = cv::Vec3b(bgra[base + 0], bgra[base + 1], bgra[base + 2]);
		}
	}
	int scale = 6;
	cv::Mat big;
	cv::resize(crop, big, cv::Size(gw * scale, gh * scale), 0, 0, cv::INTER_NEAREST);
	std::vector<int> params;
	params.push_back(cv::IMWRITE_PNG_COMPRESSION);
	params.push_back(3);
	cv::imencode(".png", big, *png, params);
}

// Encode the entire region crop (regular pixels) to a PNG, scaled up for display.
static void TFE_FullRegionPng(const unsigned char *bgra, int w, int h, std::vector<unsigned char> *png)
{
	png->clear();
	if (w < 1 || h < 1) return;
	cv::Mat crop(h, w, CV_8UC3);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int base = (y * w + x) * 4;
			crop.at<cv::Vec3b>(y, x) = cv::Vec3b(bgra[base + 0], bgra[base + 1], bgra[base + 2]);
		}
	}
	int scale = 3;
	cv::Mat big;
	cv::resize(crop, big, cv::Size(w * scale, h * scale), 0, 0, cv::INTER_NEAREST);
	std::vector<int> params;
	params.push_back(cv::IMWRITE_PNG_COMPRESSION);
	params.push_back(3);
	cv::imencode(".png", big, *png, params);
}

// ===========================================================================
// CTrainerFonts
// ===========================================================================

CTrainerFonts::CTrainerFonts()
{
	InitializeCriticalSection(&_cs);
	for (int i = 0; i < TFE_NUM_FONT_GROUPS; i++) _scanmode[i] = "plain";
}

CTrainerFonts::~CTrainerFonts()
{
	DeleteCriticalSection(&_cs);
}

static bool IsTRecordToken(const CString &token, int *group_out)
{
	// "t<digit>$<char>" - the first whitespace token of a t$ line.
	if (token.GetLength() < 4) return false;
	if (token[0] != 't') return false;
	if (token[1] < '0' || token[1] > '9') return false;
	if (token[2] != '$') return false;
	if (group_out) *group_out = token[1] - '0';
	return true;
}

bool CTrainerFonts::LoadFromTablemap(const CString &tm_path)
{
	EnterCriticalSection(&_cs);
	_tm_path = tm_path;
	for (int i = 0; i < TFE_NUM_FONT_GROUPS; i++) { _groups[i].clear(); _scanmode[i] = "plain"; }

	CStdioFile file;
	if (!file.Open(tm_path, CFile::modeRead | CFile::typeText)) {
		LeaveCriticalSection(&_cs);
		return false;
	}
	CString line;
	while (file.ReadString(line)) {
		int pos = 0;
		CString first = line.Tokenize(" \t", pos);
		if (first.IsEmpty()) continue;

		int group = -1;
		if (IsTRecordToken(first, &group)) {
			TFontRec rec;
			rec.ch = (char)first[3];
			rec.x_count = 0;
			rec.hexmash = "";
			while (pos != -1 && rec.x_count < TFE_MAX_SINGLE_CHAR_WIDTH) {
				CString tok = line.Tokenize(" \t", pos);
				if (tok.IsEmpty()) break;
				rec.x[rec.x_count++] = strtoul(CStringA(tok).GetString(), NULL, 16);
				rec.hexmash += CStringA(tok);
			}
			if (rec.x_count > 0)
				_groups[group][rec.hexmash] = rec;
			continue;
		}

		// s$t<digit>type <plain|fuzzy|number>
		if (first.Left(2) == "s$") {
			CString name = first.Mid(2);            // e.g. "t3type"
			if (name.GetLength() == 6 && name[0] == 't' && name[1] >= '0' && name[1] <= '9'
				&& name.Mid(2).CompareNoCase("type") == 0) {
				CString rest = line.Mid(first.GetLength());
				rest.Trim();
				if (!rest.IsEmpty())
					_scanmode[name[1] - '0'] = CStringA(rest);
			}
		}
	}
	file.Close();
	LeaveCriticalSection(&_cs);
	return true;
}

// SQL single-quote escaping.
static CStringA FontsSqlEsc(const CStringA &s)
{
	CStringA out;
	for (int i = 0; i < s.GetLength(); ++i) {
		char c = s[i];
		if (c == '\'') out += "''"; else out += c;
	}
	return out;
}

// Load the t$ font groups + s$tXtype scan modes for a tablemap from the
// PostgreSQL "hiss" database. tm_name is the tablemap's DB key.
bool CTrainerFonts::LoadFromDB(const CString &tm_name)
{
	EnterCriticalSection(&_cs);
	_tm_path = tm_name;   // member now holds the DB tablemap name
	for (int i = 0; i < TFE_NUM_FONT_GROUPS; i++) { _groups[i].clear(); _scanmode[i] = "plain"; }

	PGconn *conn = TrainerDB_Connect();
	if (conn == NULL) { LeaveCriticalSection(&_cs); return false; }
	long id = TrainerDB_TablemapId(conn, tm_name);
	if (id < 0) { PQfinish(conn); LeaveCriticalSection(&_cs); return false; }

	// Fonts.
	CString sql;
	sql.Format("SELECT font_group, ch, x_values FROM tm_fonts WHERE tablemap_id=%ld", id);
	PGresult *res = PQexec(conn, sql.GetString());
	if (PQresultStatus(res) == PGRES_TUPLES_OK) {
		int rows = PQntuples(res);
		for (int i = 0; i < rows; ++i) {
			int group = atoi(PQgetvalue(res, i, 0));
			if (group < 0 || group >= TFE_NUM_FONT_GROUPS) continue;
			CStringA ch = PQgetvalue(res, i, 1);
			CStringA xvals = PQgetvalue(res, i, 2);
			TFontRec rec;
			rec.ch = ch.IsEmpty() ? ' ' : ch[0];
			rec.x_count = 0;
			rec.hexmash = "";
			int pos = 0;
			while (pos != -1 && rec.x_count < TFE_MAX_SINGLE_CHAR_WIDTH) {
				CStringA tok = xvals.Tokenize(" \t", pos);
				if (tok.IsEmpty()) break;
				rec.x[rec.x_count++] = strtoul(tok.GetString(), NULL, 16);
				rec.hexmash += tok;
			}
			if (rec.x_count > 0)
				_groups[group][rec.hexmash] = rec;
		}
	}
	if (res) PQclear(res);

	// Scan modes: s$t<digit>type symbols.
	sql.Format("SELECT name, text FROM tm_symbols WHERE tablemap_id=%ld AND name LIKE 't%%type'", id);
	res = PQexec(conn, sql.GetString());
	if (PQresultStatus(res) == PGRES_TUPLES_OK) {
		int rows = PQntuples(res);
		for (int i = 0; i < rows; ++i) {
			CString name = PQgetvalue(res, i, 0);   // e.g. "t3type"
			CStringA val = PQgetvalue(res, i, 1);
			if (name.GetLength() == 6 && name[0] == 't' && name[1] >= '0' && name[1] <= '9'
					&& name.Mid(2).CompareNoCase("type") == 0 && !val.IsEmpty()) {
				_scanmode[name[1] - '0'] = val;
			}
		}
	}
	if (res) PQclear(res);

	PQfinish(conn);
	LeaveCriticalSection(&_cs);
	return true;
}

// Replace the loaded tablemap's t$ records in the database with the current
// in-memory font groups (transactional delete + re-insert).
bool CTrainerFonts::SaveToDB()
{
	EnterCriticalSection(&_cs);
	CString name = _tm_path;
	if (name.IsEmpty()) { LeaveCriticalSection(&_cs); return false; }

	PGconn *conn = TrainerDB_Connect();
	if (conn == NULL) { LeaveCriticalSection(&_cs); return false; }
	long id = TrainerDB_TablemapId(conn, name);
	if (id < 0) { PQfinish(conn); LeaveCriticalSection(&_cs); return false; }

	bool ok = true;
	PGresult *res = PQexec(conn, "BEGIN");
	ok = ok && (PQresultStatus(res) == PGRES_COMMAND_OK);
	if (res) PQclear(res);

	if (ok) {
		CString sql;
		sql.Format("DELETE FROM tm_fonts WHERE tablemap_id=%ld", id);
		res = PQexec(conn, sql.GetString());
		ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
		if (res) PQclear(res);
	}

	for (int g = 0; ok && g < TFE_NUM_FONT_GROUPS; g++) {
		for (TFontGroup::const_iterator it = _groups[g].begin(); ok && it != _groups[g].end(); ++it) {
			CStringA xvals;
			for (int j = 0; j < it->second.x_count; j++) {
				CStringA hx; hx.Format(j == 0 ? "%x" : " %x", it->second.x[j]);
				xvals += hx;
			}
			CStringA ch; ch += it->second.ch;
			CString sql;
			sql.Format("INSERT INTO tm_fonts (tablemap_id,font_group,ch,hexmash,x_values) "
				"VALUES (%ld,%d,'%s','%s','%s')",
				id, g, FontsSqlEsc(ch).GetString(),
				FontsSqlEsc(it->second.hexmash).GetString(), FontsSqlEsc(xvals).GetString());
			res = PQexec(conn, sql.GetString());
			ok = (PQresultStatus(res) == PGRES_COMMAND_OK);
			if (res) PQclear(res);
		}
	}

	res = PQexec(conn, ok ? "COMMIT" : "ROLLBACK");
	if (res) PQclear(res);
	PQfinish(conn);
	LeaveCriticalSection(&_cs);
	return ok;
}

CString CTrainerFonts::RecognizeBgra(const unsigned char *bgra, int w, int h,
	COLORREF color, int radius, int group)
{
	if (bgra == NULL || group < 0 || group >= TFE_NUM_FONT_GROUPS) return "";
	if (w < 1 || h < 1 || w > TFE_MAX_CHAR_WIDTH || h > TFE_MAX_CHAR_HEIGHT) return "";

	std::vector<unsigned char> ch, bg;
	int stride = 0;
	TFE_BuildCharArray(bgra, w, h, color, radius, &ch, &bg, &stride);

	EnterCriticalSection(&_cs);
	CStringA mode = _scanmode[group];
	TFontGroup groupCopy = _groups[group];   // copy under lock; scan outside not needed but cheap
	LeaveCriticalSection(&_cs);

	CStringA modeLower = mode; modeLower.MakeLower();
	CString text;
	if (modeLower != "fuzzy" && atof(mode.GetString()) == 0)
		text = TFE_PlainScan(groupCopy, w, h, &bg[0], &ch[0], stride);
	else
		text = TFE_FuzzyScan(groupCopy, w, h, &bg[0], &ch[0], stride,
			modeLower == "fuzzy" ? TFE_DEFAULT_FUZZY_TOLERANCE : atof(mode.GetString()));
	return text;
}

void CTrainerFonts::SegmentBgra(const unsigned char *bgra, int w, int h,
	COLORREF color, int radius, int group, std::vector<TGlyph> *out)
{
	if (out == NULL) return;
	out->clear();
	if (bgra == NULL || w < 1 || h < 1 || w > TFE_MAX_CHAR_WIDTH || h > TFE_MAX_CHAR_HEIGHT) return;

	std::vector<unsigned char> ch, bg;
	int stride = 0;
	TFE_BuildCharArray(bgra, w, h, color, radius, &ch, &bg, &stride);

	// The entire region scrape (regular pixels), shared by every glyph from it.
	std::vector<unsigned char> full_png;
	TFE_FullRegionPng(bgra, w, h, &full_png);

	int scan_pos = 0;
	while (scan_pos < w && bg[scan_pos]) scan_pos++;
	while (scan_pos < w) {
		int start = scan_pos;
		while (scan_pos < w && !bg[scan_pos]) scan_pos++;   // foreground band [start, scan_pos)

		int xb = 0, xe = 0, yb = 0, ye = 0;
		TFE_GetShiftLeftDownIndexes(start, scan_pos - start, h, &bg[0], &ch[0], stride, &xb, &xe, &yb, &ye);

		TGlyph glyph;
		CStringA spaced;
		TFE_CalcHexmash(xb, xe, yb, ye, &ch[0], stride, &spaced, true);
		glyph.x_count = 0;
		int p = 0;
		while (p != -1 && glyph.x_count < TFE_MAX_SINGLE_CHAR_WIDTH) {
			CStringA num = spaced.Tokenize(" ", p);
			if (num.IsEmpty()) break;
			glyph.x[glyph.x_count++] = strtoul(num.GetString(), NULL, 16);
		}
		TFE_CalcHexmash(xb, xe, yb, ye, &ch[0], stride, &glyph.hexmash, false);

		EnterCriticalSection(&_cs);
		TFontGroup::const_iterator it = (group >= 0 && group < TFE_NUM_FONT_GROUPS)
			? _groups[group].find(glyph.hexmash) : _groups[0].end();
		glyph.existing_ch = (group >= 0 && group < TFE_NUM_FONT_GROUPS && it != _groups[group].end())
			? it->second.ch : 0;
		LeaveCriticalSection(&_cs);

		if (glyph.x_count > 0) {
			glyph.xb = xb; glyph.xe = xe; glyph.yb = yb; glyph.ye = ye;
			TFE_GlyphToPng(glyph, &glyph.png);
			TFE_RegularGlyphPng(bgra, w, h, xb, xe, yb, ye, &glyph.regular_png);
			glyph.full_png = full_png;
			out->push_back(glyph);
		}

		while (scan_pos < w && bg[scan_pos]) scan_pos++;
	}
}

bool CTrainerFonts::RegenGlyphAt(const unsigned char *bgra, int w, int h,
	COLORREF color, int radius, int group, int center_x, TGlyph *out)
{
	if (out == NULL) return false;
	*out = TGlyph();
	out->x_count = 0;
	out->existing_ch = 0;
	// Always provide the full region image so the editor still shows the scrape
	// even when the new colour cube produces no foreground at this position.
	TFE_FullRegionPng(bgra, w, h, &out->full_png);

	std::vector<TGlyph> glyphs;
	SegmentBgra(bgra, w, h, color, radius, group, &glyphs);
	if (glyphs.empty()) return false;

	int best = -1, best_dist = 0x7fffffff;
	for (size_t i = 0; i < glyphs.size(); ++i) {
		if (center_x >= glyphs[i].xb && center_x <= glyphs[i].xe) { best = (int)i; break; }
		int gc = (glyphs[i].xb + glyphs[i].xe) / 2;
		int d = gc > center_x ? gc - center_x : center_x - gc;
		if (d < best_dist) { best_dist = d; best = (int)i; }
	}
	if (best < 0) return false;
	*out = glyphs[best];
	return true;
}

void CTrainerFonts::MaskPng(const unsigned char *bgra, int w, int h, COLORREF color, int radius,
	std::vector<unsigned char> *png)
{
	png->clear();
	if (bgra == NULL || w < 1 || h < 1 || w > TFE_MAX_CHAR_WIDTH || h > TFE_MAX_CHAR_HEIGHT) return;
	std::vector<unsigned char> ch, bg;
	int stride = 0;
	TFE_BuildCharArray(bgra, w, h, color, radius, &ch, &bg, &stride);
	cv::Mat mask(h, w, CV_8UC1, cv::Scalar(0));
	for (int x = 0; x < w; x++)
		for (int y = 0; y < h; y++)
			if (ch[x * stride + y]) mask.at<unsigned char>(y, x) = 255;
	int scale = 3;
	cv::Mat big, bgr;
	cv::resize(mask, big, cv::Size(w * scale, h * scale), 0, 0, cv::INTER_NEAREST);
	cv::cvtColor(big, bgr, cv::COLOR_GRAY2BGR);
	std::vector<int> params;
	params.push_back(cv::IMWRITE_PNG_COMPRESSION);
	params.push_back(3);
	cv::imencode(".png", bgr, *png, params);
}

void CTrainerFonts::SetGlyph(int group, const TGlyph &glyph, char ch)
{
	if (group < 0 || group >= TFE_NUM_FONT_GROUPS || glyph.x_count <= 0) return;
	EnterCriticalSection(&_cs);
	TFontRec rec;
	rec.ch = ch;
	rec.x_count = glyph.x_count;
	for (int j = 0; j < glyph.x_count; j++) rec.x[j] = glyph.x[j];
	rec.hexmash = glyph.hexmash;
	_groups[group][glyph.hexmash] = rec;
	LeaveCriticalSection(&_cs);
}

void CTrainerFonts::RemoveHexmash(int group, const CStringA &hexmash)
{
	if (group < 0 || group >= TFE_NUM_FONT_GROUPS) return;
	EnterCriticalSection(&_cs);
	_groups[group].erase(hexmash);
	LeaveCriticalSection(&_cs);
}

int CTrainerFonts::RemoveCharFromGroup(int group, char ch, std::vector<TFontRec> *removed_out)
{
	if (group < 0 || group >= TFE_NUM_FONT_GROUPS || ch == 0) return 0;
	EnterCriticalSection(&_cs);
	// A character can have several font records (different glyph shapes), each keyed
	// by hexmash. Collect all whose record maps to this char (keeping a copy for undo),
	// then erase them.
	std::vector<CStringA> keys;
	for (TFontGroup::const_iterator it = _groups[group].begin(); it != _groups[group].end(); ++it) {
		if (it->second.ch == ch) {
			keys.push_back(it->first);
			if (removed_out) removed_out->push_back(it->second);
		}
	}
	for (size_t i = 0; i < keys.size(); i++) _groups[group].erase(keys[i]);
	int count = (int)keys.size();
	LeaveCriticalSection(&_cs);
	// Persist the deletion to the hiss database (full font-set sync, same path as a
	// save). Report failure so the UI doesn't claim success on a DB error.
	if (count > 0 && !SaveToDB()) {
		if (removed_out) removed_out->clear();
		return -1;
	}
	return count;
}

bool CTrainerFonts::RestoreFonts(int group, const std::vector<TFontRec> &recs)
{
	if (group < 0 || group >= TFE_NUM_FONT_GROUPS) return false;
	EnterCriticalSection(&_cs);
	for (size_t i = 0; i < recs.size(); i++) _groups[group][recs[i].hexmash] = recs[i];
	LeaveCriticalSection(&_cs);
	return SaveToDB();
}

int CTrainerFonts::group_count(int g)
{
	if (g < 0 || g >= TFE_NUM_FONT_GROUPS) return 0;
	EnterCriticalSection(&_cs);
	int n = (int)_groups[g].size();
	LeaveCriticalSection(&_cs);
	return n;
}

// All distinct characters that currently have at least one font record in group g
// (used by the editor's "unaccounted glyphs" coverage footer).
CStringA CTrainerFonts::GroupChars(int g)
{
	CStringA out;
	if (g < 0 || g >= TFE_NUM_FONT_GROUPS) return out;
	EnterCriticalSection(&_cs);
	for (TFontGroup::const_iterator it = _groups[g].begin(); it != _groups[g].end(); ++it) {
		char c = it->second.ch;
		if (c != 0 && out.Find(c) < 0) out += c;
	}
	LeaveCriticalSection(&_cs);
	return out;
}

bool CTrainerFonts::HasHexmash(int g, const CStringA &hexmash)
{
	if (g < 0 || g >= TFE_NUM_FONT_GROUPS) return false;
	EnterCriticalSection(&_cs);
	bool has = (_groups[g].find(hexmash) != _groups[g].end());
	LeaveCriticalSection(&_cs);
	return has;
}

CStringA CTrainerFonts::scan_mode(int g)
{
	if (g < 0 || g >= TFE_NUM_FONT_GROUPS) return "plain";
	EnterCriticalSection(&_cs);
	CStringA m = _scanmode[g];
	LeaveCriticalSection(&_cs);
	return m;
}

int CTrainerFonts::DeleteAllFonts()
{
	EnterCriticalSection(&_cs);
	int count = 0;
	for (int g = 0; g < TFE_NUM_FONT_GROUPS; g++) {
		count += (int)_groups[g].size();
		_groups[g].clear();
	}
	LeaveCriticalSection(&_cs);
	// Persist the now-empty font set to the hiss database (deletes every tm_fonts
	// row for this tablemap). Report failure (-1) so the UI doesn't claim success
	// when no tablemap is loaded or the DB write failed.
	if (!SaveToDB()) {
		return -1;
	}
	return count;
}

bool CTrainerFonts::SaveToTablemap()
{
	EnterCriticalSection(&_cs);
	CString path = _tm_path;
	if (path.IsEmpty()) { LeaveCriticalSection(&_cs); return false; }

	// Read every line (text mode strips EOLs).
	std::vector<CStringA> lines;
	{
		CStdioFile file;
		if (!file.Open(path, CFile::modeRead | CFile::typeText)) { LeaveCriticalSection(&_cs); return false; }
		CString line;
		while (file.ReadString(line)) lines.push_back(CStringA(line));
		file.Close();
	}

	// Build the regenerated t$ block from the in-memory groups.
	std::vector<CStringA> tblock;
	for (int g = 0; g < TFE_NUM_FONT_GROUPS; g++) {
		for (TFontGroup::const_iterator it = _groups[g].begin(); it != _groups[g].end(); ++it) {
			CStringA l;
			l.Format("t%d$%c", g, it->second.ch);
			for (int j = 0; j < it->second.x_count; j++) {
				CStringA hx; hx.Format(" %x", it->second.x[j]);
				l += hx;
			}
			tblock.push_back(l);
		}
	}

	// Rewrite: keep all non-t$ lines; inject the regenerated block where the first
	// t$ line was. If there were no t$ lines, append a fonts section at the end.
	std::vector<CStringA> out;
	bool injected = false;
	for (size_t i = 0; i < lines.size(); ++i) {
		int pos = 0;
		CString first = CString(lines[i]).Tokenize(" \t", pos);
		int grp = -1;
		if (IsTRecordToken(first, &grp)) {
			if (!injected) { for (size_t k = 0; k < tblock.size(); ++k) out.push_back(tblock[k]); injected = true; }
			continue;   // drop the old t$ line
		}
		out.push_back(lines[i]);
	}
	if (!injected) {
		out.push_back("");
		out.push_back("// fonts");
		for (size_t k = 0; k < tblock.size(); ++k) out.push_back(tblock[k]);
	}

	// Write back with CRLF.
	FILE *fp = NULL;
	if (fopen_s(&fp, CStringA(path).GetString(), "wb") != 0 || fp == NULL) {
		LeaveCriticalSection(&_cs);
		return false;
	}
	for (size_t i = 0; i < out.size(); ++i) {
		fwrite(out[i].GetString(), 1, out[i].GetLength(), fp);
		fwrite("\r\n", 1, 2, fp);
	}
	fclose(fp);
	LeaveCriticalSection(&_cs);
	return true;
}
