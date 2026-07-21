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

#ifndef INC_CHEARTBEATTHREAD_H
#define INC_CHEARTBEATTHREAD_H

#include "CHeartbeatDelay.h"
#include "COpenHoldemStarter.h"
#include "CSpaceOptimizedGlobalObject.h"
#include "CWatchdog.h"

class CHeartbeatThread /*: public CSpaceOptimizedGlobalObject */{
 public:
	// public functions
	CHeartbeatThread();
	~CHeartbeatThread();
 public:
	// public accessors
	void StartThread();
	long int heartbeat_counter() { return _heartbeat_counter; }
 public:
	// This critical section does not control access to any variables/members, but is used as
	// a flag to indicate when the scraper/symbol classes are in an update cycle
	static CRITICAL_SECTION	cs_update_in_progress;
	// 1 only while cs_update_in_progress is actually INITIALIZED (between this object's ctor and
	// dtor). The HTTP server thread outlives that window at BOTH ends, and /api/symbols takes this
	// lock -- entering it before InitializeCriticalSection (or after DeleteCriticalSection) corrupts
	// it and later blew up the heartbeat inside RtlEnterCriticalSection (crash_hiss_23456).
	// Off-thread users MUST check this before Enter/Leave.
	static volatile LONG cs_update_ready;
 private:
	// private functions and variables - not available via accessors or mutators
	static UINT HeartbeatThreadFunction(LPVOID pParam);
	static void ScrapeEvaluateAct();
	// Publishes g_seat_state / g_seat_evidence / g_seat_since_tick for /api/seat-status.
	static void UpdateSeatStatus();
	static void AutoConnect();
 private:
	// private variables - use public accessors and public mutators to address these	
	CCritSec	m_critsec;
	static		CHeartbeatThread *pParent;
	static		long int _heartbeat_counter;
 private:
  static CHeartbeatDelay _heartbeat_delay;
  static COpenHoldemStarter _openholdem_starter;
 private:
	HANDLE		_m_stop_thread;
	HANDLE		_m_wait_thread;
};

extern CHeartbeatThread *p_heartbeat_thread;

#endif //INC_CHEARTBEATTHREAD_H