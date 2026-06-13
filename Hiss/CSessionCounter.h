#pragma once
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

#include "CSpaceOptimizedGlobalObject.h"

class CSessionCounter {
 public:
	// public functions
	CSessionCounter();
	// Worker-mode ctor: an OCR worker process must NOT consume one of the limited
	// session-ID mutex slots (that would exhaust the counter and make later
	// launches fail). It still needs a non-NULL p_sessioncounter with a valid
	// session_id() so singletons like the watchdog don't crash; pass
	// worker_mode=true to take a fixed id (0) without grabbing any mutex.
	explicit CSessionCounter(bool worker_mode);
	~CSessionCounter();
 public:
	// public accessors
	// session_id() returns a value in the range 0..(MAX_SESSION_IDS - 1)
	int session_id() { return _session_id; }
 private:
	// private variables - use public accessors and public mutators to address these
	int _session_id;
 private:
	// private functions and variables - not available via accessors or mutators		
	HANDLE hMutex;
};

extern CSessionCounter *p_sessioncounter;
