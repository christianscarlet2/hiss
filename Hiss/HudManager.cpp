#include "stdafx.h"
#include "HudManager.h"

#include <math.h>
#include <float.h>
#include "CPokerTrackerThread.h"
#include "CHandHistoryWriter.h"         // p_handhistory_writer -> stabilized per-seat "main name"
#include "CEngineContainer.h"           // p_engine_container -> current gametype for per-gametype HUD
#include "CSymbolEngineIsOmaha.h"
#include "CTableState.h"
#include "..\CTablemap\CTablemapDB.h"   // own-data HUD: hud_player_stats (replaces PT4)
#include "..\PokerTracker_Query_Definitions\pokertracker_query_definitions.h"
#include "..\Shared\MagicNumbers\MagicNumbers.h"

CHudManager *p_hud_manager = NULL;

const char *kDefaultHudProfilePath = "C:\\Users\\scarl\\Downloads\\Cash - BlackRain79 HUD.pt4hud";
const size_t kMaxHudStats = 16;

CHudManager::CHudManager()
{
	_enabled = false;
	_last_refresh_tick = 0;
	for (int chair = 0; chair < kMaxNumberOfPlayers; ++chair) {
		_chair_samples[chair] = -1;
	}
	CString message;
	LoadProfileFromPath(kDefaultHudProfilePath, &message, false);
}

bool CHudManager::IsEnabled(void) const
{
	return _enabled && !_definitions.empty();
}

CString CHudManager::ProfilePath(void) const
{
	return _profile_path;
}

std::vector<SHudStatValue> CHudManager::StatsForChair(int chair) const
{
	CSingleLock lock(&_mutex, TRUE);
	if (chair < 0 || chair >= kMaxNumberOfPlayers) {
		return std::vector<SHudStatValue>();
	}
	return _chair_stats[chair];  // copy under lock; caller iterates safely
}

int CHudManager::SamplesForChair(int chair) const
{
	CSingleLock lock(&_mutex, TRUE);
	if (chair < 0 || chair >= kMaxNumberOfPlayers) {
		return -1;
	}
	return _chair_samples[chair];
}

bool CHudManager::LoadProfile(CString path, CString *message)
{
	return LoadProfileFromPath(path, message, true);
}

bool CHudManager::LoadProfileFromPath(CString path, CString *message, bool refresh_now)
{
	std::vector<SHudStatDefinition> loaded;
	CString lower_path = path;
	lower_path.MakeLower();

	if (lower_path.Right(7) == ".pt4hud") {
		LoadPt4HudProfile(path, &loaded);
	}
	else {
		CStdioFile file;
		if (!file.Open(path, CFile::modeRead | CFile::typeText)) {
			if (message != NULL) {
				message->Format("Could not open HUD profile: %s", path.GetString());
			}
			return false;
		}

		CString line;
		while (file.ReadString(line)) {
			SHudStatDefinition definition;
			if (ParseProfileLine(line, &definition)) {
				if (AddDefinition(&loaded, definition.symbol, definition.full_name) && !loaded.empty()) {
					loaded.back().abbreviation = definition.abbreviation;
					loaded.back().important = definition.important;
				}
			}
		}
		file.Close();
	}

	if (loaded.empty()) {
		LoadDefaultProfile();
		if (message != NULL) {
			message->Format("HUD profile had no recognized stats. Loaded default HUD profile instead.");
		}
	}
	else {
		_definitions = loaded;
		if (message != NULL) {
			message->Format("Loaded HUD profile with %d stats.", (int)_definitions.size());
		}
	}

	_profile_path = path;
	_enabled = true;
	_last_refresh_tick = 0;
	_last_hand_number = "";
	if (refresh_now) {
		RefreshIfNeeded("", true);
	}
	return true;
}

bool CHudManager::AddDefinition(std::vector<SHudStatDefinition> *loaded, CString symbol, CString full_name) const
{
	if (loaded == NULL || symbol.IsEmpty()) {
		return false;
	}
	if (loaded->size() >= kMaxHudStats) {
		return false;
	}
	for (size_t i = 0; i < loaded->size(); ++i) {
		if ((*loaded)[i].symbol.CompareNoCase(symbol) == 0) {
			return false;
		}
	}
	SHudStatDefinition definition;
	definition.symbol = symbol;
	definition.abbreviation = AbbreviationFor(symbol);
	definition.full_name = full_name.IsEmpty() ? DescriptionFor(symbol) : full_name;
	definition.important = ImportantByDefault(symbol);
	loaded->push_back(definition);
	return true;
}

bool CHudManager::LoadPt4HudProfile(CString path, std::vector<SHudStatDefinition> *loaded) const
{
	CFile file;
	if (!file.Open(path, CFile::modeRead | CFile::typeBinary)) {
		return false;
	}
	ULONGLONG length = file.GetLength();
	if (length == 0 || length > 1024 * 1024 * 8) {
		file.Close();
		return false;
	}
	CByteArray bytes;
	bytes.SetSize((INT_PTR)length);
	file.Read(bytes.GetData(), (UINT)length);
	file.Close();

	for (int offset = 0; offset < 2; ++offset) {
		CString current;
		for (INT_PTR i = offset; i + 1 < bytes.GetSize(); i += 2) {
			unsigned short raw = (unsigned char)bytes[i] | ((unsigned char)bytes[i + 1] << 8);
			char ch = (char)raw;
			bool printable = raw >= 32 && raw <= 126;
			if (printable) {
				current += ch;
				continue;
			}
			if (current.GetLength() >= 2) {
				CString candidate = current;
				candidate.Trim();
				CString symbol = NormalizeSymbol(candidate);
				if (!symbol.IsEmpty()) {
					AddDefinition(loaded, symbol, candidate);
				}
			}
			current = "";
		}
	}
	return loaded != NULL && !loaded->empty();
}

void CHudManager::LoadDefaultProfile(void)
{
	const char *defaults[] = { "hands", "vpip", "pfr", "threebet", "aggfreq", "wtsd" };
	_definitions.clear();
	for (int i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
		CString symbol = NormalizeSymbol(defaults[i]);
		if (symbol.IsEmpty()) {
			continue;
		}
		SHudStatDefinition definition;
		definition.symbol = symbol;
		definition.abbreviation = AbbreviationFor(symbol);
		definition.full_name = DescriptionFor(symbol);
		definition.important = ImportantByDefault(symbol);
		AddDefinition(&_definitions, definition.symbol, definition.full_name);
	}
}

bool CHudManager::ParseProfileLine(CString line, SHudStatDefinition *definition) const
{
	line.Trim();
	if (line.IsEmpty() || line.Left(1) == "#" || line.Left(1) == ";") {
		return false;
	}

	CString parts[4];
	int count = 0;
	int start = 0;
	for (int i = 0; i <= line.GetLength() && count < 4; ++i) {
		if (i == line.GetLength() || line[i] == '|' || line[i] == ',' || line[i] == '=') {
			parts[count++] = line.Mid(start, i - start);
			start = i + 1;
		}
	}
	for (int i = 0; i < count; ++i) {
		parts[i].Trim();
	}

	CString symbol = NormalizeSymbol(parts[0]);
	if (symbol.IsEmpty()) {
		return false;
	}

	definition->symbol = symbol;
	definition->abbreviation = AbbreviationFor(symbol);
	definition->full_name = DescriptionFor(symbol);
	definition->important = ImportantByDefault(symbol);

	if (count >= 2 && !parts[1].IsEmpty()) {
		CString second = parts[1];
		CString second_lower = second;
		second_lower.MakeLower();
		if (second_lower == "important" || second_lower == "white" || second_lower == "1" || second_lower == "true") {
			definition->important = true;
		}
		else {
			definition->full_name = second;
		}
	}
	if (count >= 3 && !parts[2].IsEmpty()) {
		CString third = parts[2];
		third.MakeLower();
		definition->important = third == "important" || third == "white" || third == "1" || third == "true";
	}
	if (definition->full_name.IsEmpty()) {
		definition->full_name = definition->abbreviation;
	}
	return true;
}

CString CHudManager::NormalizeSymbol(CString text) const
{
	text.Trim();
	if (text.IsEmpty()) {
		return "";
	}
	text.MakeLower();
	text.Replace(":", "");
	text.Replace("+", "");
	text.Replace("$", "");
	text.Replace("%", "");
	text.Replace("/", "");
	text.Replace(" ", "");
	text.Replace("-", "_");
	if (text == "h" || text == "hand" || text == "handsabbreviated") return "hands";
	if (text == "vp" || text == "vpi" || text == "vpip") return "vpip";
	if (text == "pf" || text == "pfr") return "pfr";
	if (text == "rfi" || text == "raisefirstin" || text.Left(10) == "raisefirst") return "rfi";
	if (text == "3b" || text == "threebet" || text == "3bet" || text == "3betpreflop" || text.Left(9) == "3betpref") return "3bet";
	if (text == "4b" || text == "4bet" || text == "4betratio") return "4bet";
	if (text == "5b" || text == "5bet" || text == "5betratio") return "5bet";
	if (text == "f3b" || text == "foldto3betafterraising" || text.Left(23) == "foldtopf3betafterrais") return "fold_to_3bet_afterR";
	if (text == "f4b" || text == "foldto4betafter3bet" || text.Left(21) == "foldtopf4betafter3be") return "fold_to_4bet";
	if (text.Left(8) == "attostea" || text == "attempttosteal" || text == "steal") return "steal_attempt";
	if (text.Left(10) == "foldtostea") return "bb_fold_to_steal";
	if (text == "totalaf" || text == "totala" || text == "af") return "total_af";
	if (text == "wtsd" || text == "wenttoshowdown") return "wtsd";
	if (text == "wsd" || text == "wonatshowdown") return "wssd";
	if (text == "wwsf" || text == "wonwhensawflop") return "flop_seen";
	if (text == "cbf" || text == "cbetflop") return "flop_cbet";
	if (text == "cbt" || text.Left(7) == "cbettur") return "turn_cbet";
	if (text == "cbr" || text == "cbetriver") return "river_bet";
	if (text == "ffcb" || text == "foldtofcbet") return "flop_fold_to_cbet";
	if (text == "ftcb" || text.Left(11) == "foldtotcbe") return "turn_fold_to_cbet";
	if (text == "frcb" || text.Left(11) == "foldtorcbe") return "river_fold_to_cbet";
	if (text.Left(3) == "pt_") {
		text = text.Mid(3);
	}

	for (int i = 0; i < PT_DLL_GetNumberOfStats(); ++i) {
		CString symbol = PT_DLL_GetBasicSymbolNameWithoutPTPrefix(i);
		CString lower_symbol = symbol;
		lower_symbol.MakeLower();
		if (text == lower_symbol) {
			return symbol;
		}
		CString description = PT_DLL_GetDescription(i);
		description.MakeLower();
		description.Replace(":", "");
		description.Replace("+", "");
		description.Replace("$", "");
		description.Replace("%", "");
		description.Replace("/", "");
		description.Replace(" ", "");
		description.Replace("-", "_");
		if (text == description) {
			return symbol;
		}
	}
	return "";
}

CString CHudManager::AbbreviationFor(CString symbol) const
{
	CString lower = symbol;
	lower.MakeLower();
	if (lower == "hands") return "H";
	if (lower == "vpip") return "VP";
	if (lower == "pfr") return "PF";
	if (lower == "rfi") return "RFI";
	if (lower == "3bet") return "3B";
	if (lower == "4bet") return "4B";
	if (lower == "5bet") return "5B";
	if (lower == "fold_to_3bet_afterr" || lower == "fold_to_3bet") return "F3B";
	if (lower == "fold_to_4bet") return "F4B";
	if (lower == "steal_attempt") return "ATS";
	if (lower == "bb_fold_to_steal") return "FTS";
	if (lower == "aggfreq" || lower == "afq") return "AFq";
	if (lower == "total_af" || lower == "af") return "AF";
	if (lower == "wtsd") return "WT";
	if (lower == "wssd") return "WSD";
	if (lower == "flop_seen") return "WWSF";
	if (lower == "flop_cbet") return "CB";
	if (lower == "turn_cbet") return "CBT";
	if (lower == "river_bet") return "CBR";
	if (lower == "flop_fold_to_cbet") return "FC";
	if (lower == "turn_fold_to_cbet") return "FTC";
	if (lower == "river_fold_to_cbet") return "FRC";

	CString abbreviation = symbol;
	abbreviation.Replace("_", "");
	if (abbreviation.GetLength() > 4) {
		abbreviation = abbreviation.Left(4);
	}
	abbreviation.MakeUpper();
	return abbreviation;
}

CString CHudManager::DescriptionFor(CString symbol) const
{
	for (int i = 0; i < PT_DLL_GetNumberOfStats(); ++i) {
		CString candidate = PT_DLL_GetBasicSymbolNameWithoutPTPrefix(i);
		if (candidate.CompareNoCase(symbol) == 0) {
			CString description = PT_DLL_GetDescription(i);
			return description.IsEmpty() ? symbol : description;
		}
	}
	return symbol;
}

bool CHudManager::ImportantByDefault(CString symbol) const
{
	CString lower = symbol;
	lower.MakeLower();
	return lower == "hands" || lower == "vpip" || lower == "pfr" ||
		lower == "3bet" || lower == "rfi" || lower == "total_af" ||
		lower == "total_afq";
}

CString CHudManager::FormatValue(CString symbol, double value) const
{
	if (value == kUndefined || !_finite(value)) {
		return "-";
	}
	CString lower = symbol;
	lower.MakeLower();
	CString text;
	if (lower == "hands") {
		text.Format("%d", (int)value);
	}
	else {
		text.Format("%.1f", value);
	}
	return text;
}

void CHudManager::RefreshIfNeeded(CString hand_number, bool force)
{
	CSingleLock lock(&_mutex, TRUE);

	DWORD now = GetTickCount();
	if (!force && hand_number == _last_hand_number && now - _last_refresh_tick < 2500) {
		return;
	}

	_last_hand_number = hand_number;
	_last_refresh_tick = now;

	// OWN-DATA HUD: stats come from hud_player_stats (postgres, fed by hud_aggregator.py from
	// the bot's logged hands) instead of PokerTracker 4. Keyed by the scraped player name.
	if (p_table_state == NULL || p_tablemap_db == NULL) {
		for (int chair = 0; chair < kMaxNumberOfPlayers; ++chair) {
			_chair_samples[chair] = -1;
			_chair_stats[chair].clear();
		}
		return;
	}

	bool build_stats = IsEnabled();
	// Per-gametype HUD: show the stats for the CURRENTLY detected game so a player's PLO read never
	// bleeds into their NLH read (and vice versa).
	CString hud_gametype = "nlhe";
	if (p_engine_container != NULL && p_engine_container->symbol_engine_isomaha() != NULL
		&& p_engine_container->symbol_engine_isomaha()->isomaha())
		hud_gametype = p_engine_container->symbol_engine_isomaha()->isplo8() ? "plo8" : "plo";
	for (int chair = 0; chair < kMaxNumberOfPlayers; ++chair) {
		_chair_samples[chair] = -1;
		_chair_stats[chair].clear();
		if (!p_table_state->Player(chair)->seated()) continue;
		// Prefer the STABILIZED per-seat name (majority vote over the last N heartbeats)
		// so the HUD keys off the same consistent identity the hand-history writer
		// records, and a single jittery frame never misses the row or hits an
		// OCR-fragment identity. Fall back to the live scrape until one is committed.
		CString name;
		if (p_handhistory_writer != NULL) name = p_handhistory_writer->StableName(chair);
		name.Trim();
		if (name.IsEmpty()) {
			name = p_table_state->Player(chair)->name();
			name.Trim();
		}
		if (name.IsEmpty()) continue;

		SHudDbStats st;
		if (!p_tablemap_db->GetHudPlayerStats(name, hud_gametype, &st) || !st.found) continue;
		_chair_samples[chair] = st.hands;
		if (!build_stats) continue;

		// PROFILE-DRIVEN: show the stats defined in the loaded HUD profile (BlackRain79),
		// each mapped to its own-data value. Stats the aggregator doesn't track yet
		// (rfi, 4bet, fold-to-4bet, turn/river cbets + folds, wsd, wwsf) are skipped until
		// hud_aggregator.py computes them. AF is a ratio (1 decimal); the rest are percents.
		for (size_t i = 0; i < _definitions.size(); ++i) {
			CString sym = _definitions[i].symbol; sym.MakeLower();
			double val = -1.0; bool pct = true;
			if      (sym == "vpip")                                   val = st.vpip;
			else if (sym == "pfr")                                    val = st.pfr;
			else if (sym == "3bet")                                   val = st.threeb;
			else if (sym == "4bet")                                   val = st.fourb;
				else if (sym == "5bet")                                   val = st.fiveb;
				else if (sym == "fold_to_3bet_afterr" || sym == "fold_to_3bet") val = st.f3b;
				else if (sym == "fold_to_4bet")                           val = st.f4b;
			else if (sym == "steal_attempt")                          val = st.steal;
			else if (sym == "bb_fold_to_steal")                       val = st.fts;
			else if (sym == "total_af" || sym == "af")              { val = st.af; pct = false; }
			else if (sym == "flop_cbet")                              val = st.cbet;
			else if (sym == "flop_fold_to_cbet")                      val = st.ftc;
			else if (sym == "wtsd")                                   val = st.wtsd;
			else if (sym == "hands")                                  continue;  // n= already shown
			// else: stat not yet computed by the aggregator -> val stays -1 -> rendered as "-"
			SHudStatValue stat;
			stat.abbreviation = _definitions[i].abbreviation;
			stat.full_name    = _definitions[i].full_name;
			stat.important    = _definitions[i].important;
			if (val < 0.0) stat.value = "-";                          // no opportunity / not tracked yet
			else stat.value.Format(pct ? "%.0f" : "%.1f", val);
			_chair_stats[chair].push_back(stat);
		}
	}
}
