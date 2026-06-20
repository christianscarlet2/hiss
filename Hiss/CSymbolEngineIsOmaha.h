//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Detecting if we play a holdem or omaha,
//   needed e.g. for automatic adaption of PT-queries.
//
//******************************************************************************

#ifndef INC_CSYMBOLENGINEISOMAHA_H
#define INC_CSYMBOLENGINEISOMAHA_H

#include "CVirtualSymbolEngine.h"

// The number of cards per player depends on the game-type.
// This affects cards to be scraped and evaluated.
// The data containers must be large enough to store kMaxNumberOfCardsPerPlayer.
int NumberOfCardsPerPlayer();

class CSymbolEngineIsOmaha: public CVirtualSymbolEngine
{
public:
	CSymbolEngineIsOmaha();
	~CSymbolEngineIsOmaha();
public:
  bool EvaluateSymbol(const CString name, double *result, bool log = false);
  CString SymbolsProvided();
public:
	// Mandatory reset-functions
	void InitOnStartup();
	void UpdateOnConnection();
	void UpdateOnHandreset();
	void UpdateOnNewRound();
	void UpdateOnMyTurn();
	void UpdateOnHeartbeat();
public:
	// Public accessors
  bool isomaha() { return _isomaha; }
  // PLO8 = Pot-Limit Omaha Hi/Lo (8-or-better). A sub-type of Omaha: the
  // hole-card scrape and isomaha() detection are identical; isplo8() additionally
  // requires the attached window title to advertise a Hi/Lo / 8-or-better game.
  // It gates the split-pot (nut-low aware) strategy branch in the OHF.
  bool isplo8() { return _isplo8; }
private:
  // Scan the attached table-window title for Hi/Lo / 8-or-better markers.
  bool TitleLooksLikeHiLo();
  bool _isomaha;
  bool _isplo8;
};

#endif INC_CSYMBOLENGINEISOMAHA_H
