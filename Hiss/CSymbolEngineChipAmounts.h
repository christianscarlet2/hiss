//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose:
//
//******************************************************************************

#ifndef INC_CSYMBOLENGINECHIPAMOUNTS_H
#define INC_CSYMBOLENGINECHIPAMOUNTS_H

#include "CVirtualSymbolEngine.h"
#include "assert.h"
#include "CSymbolEngineUserChair.h"
#include "CSymbolEngineTableLimits.h"
#include "CTableState.h"


class CSymbolEngineChipAmounts: public CVirtualSymbolEngine
{
public:
	CSymbolEngineChipAmounts();
	~CSymbolEngineChipAmounts();
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
	bool EvaluateSymbol(const CString name, double *result, bool log = false);
  CString SymbolsProvided();
public:
	double maxbalance()					{ return _maxbalance; }
	double balanceatstartofsession()	{ return _balanceatstartofsession; }
public:
	double stack(int nth_best) {
		assert(nth_best >= 0);
		assert(nth_best < kMaxNumberOfPlayers);
		return _stack[nth_best];
	}

	double stacks_at_hand_start(const int chair) { 
		assert((chair >= 0) || (chair == kUndefined));
		assert(chair < kMaxNumberOfPlayers);
		if (chair == kUndefined)	{
			return 0;
		}
		return _stacks_at_hand_start[chair]; 
	}
  double ncurrentbets();
 public:
	double pot()		    	{ return _pot; }
	double potcommon()		{ return _potcommon; }
	double potplayer()		{ return _potplayer; }
 public:
	double sraiprev()	  	{ return _sraiprev; }
 public:
	double call()		    	{ return _call;			}
	double nbetstocall()	{ return _nbetstocall;	}
	double nbetstorais()	{ return _nbetstorais; 	}
	double ncallbets()		{ return _ncallbets;	}
	double nraisbets()		{ return _nraisbets;	}
 public:
  double SortedBalance(const int rank);
  double MaxActiveOpponentStack();
 private:
	// private setters
	void SetBalance(const int player, const double d);
	void SetMaxBalanceConditionally();
	void SetBalanceAtStartOfSessionConditionally();
 private:
	void CalculateStacks();
	void CalculatePots();
	void CalculateAmountsToCallToRaise();
	void CalculateBetsToCallToRaise();
 private:
	double Largestbet();
 private:			
	double _maxbalance;
	double _balanceatstartofsession;
	double _stack[kMaxNumberOfPlayers];	
 private:
	// Used in ICM calculator - ICM needs stacks at beginning of hand
	double _stacks_at_hand_start[kMaxNumberOfPlayers];	
	double _pot; 
	double _potcommon;
	double _potplayer;
 private:
	// The last amount-to-call we actually READ on this street, while a Call button was live.
	// _call is inferred from the opponents' bet pills, so one mis-scraped pill collapses it to 0 and
	// the strategy concludes "it's free -> check" -- into a bar that has no Check button. When the
	// buttons contradict the inference we restore this value rather than inventing one.
	// See CalculateAmountsToCallToRaise().
	double _last_good_call;
	int    _last_good_call_betround;
 private:
	double _sraiprev;
	double _sraimin;
	double _sraimax;
 private:
	double _call;
	double _nbetstocall;
	double _nbetstorais;
	double _ncallbets;
	double _nraisbets;
};

#endif INC_CSYMBOLENGINECHIPAMOUNTS_H