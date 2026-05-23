#ifndef INC_HUD_MANAGER_H
#define INC_HUD_MANAGER_H

#include <vector>
#include "..\Shared\MagicNumbers\MagicNumbers.h"

struct SHudStatDefinition {
	CString symbol;
	CString abbreviation;
	CString full_name;
	bool important;
};

struct SHudStatValue {
	CString abbreviation;
	CString full_name;
	CString value;
	bool important;
};

class CHudManager {
public:
	CHudManager();
	bool LoadProfile(CString path, CString *message = NULL);
	bool IsEnabled(void) const;
	CString ProfilePath(void) const;
	const std::vector<SHudStatValue> &StatsForChair(int chair) const;
	void RefreshIfNeeded(CString hand_number, bool force = false);

private:
	CString NormalizeSymbol(CString text) const;
	CString AbbreviationFor(CString symbol) const;
	CString DescriptionFor(CString symbol) const;
	bool ImportantByDefault(CString symbol) const;
	bool ParseProfileLine(CString line, SHudStatDefinition *definition) const;
	void LoadDefaultProfile(void);
	CString FormatValue(CString symbol, double value) const;
	bool LoadProfileFromPath(CString path, CString *message, bool refresh_now);
	bool LoadPt4HudProfile(CString path, std::vector<SHudStatDefinition> *loaded) const;
	bool AddDefinition(std::vector<SHudStatDefinition> *loaded, CString symbol, CString full_name = "") const;

	bool _enabled;
	CString _profile_path;
	CString _last_hand_number;
	DWORD _last_refresh_tick;
	std::vector<SHudStatDefinition> _definitions;
	std::vector<SHudStatValue> _chair_stats[kMaxNumberOfPlayers];
};

extern CHudManager *p_hud_manager;

#endif // INC_HUD_MANAGER_H
