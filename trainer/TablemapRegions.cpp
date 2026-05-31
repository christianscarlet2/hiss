#include "stdafx.h"
#include "TablemapRegions.h"

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
