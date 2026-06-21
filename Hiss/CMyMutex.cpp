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

#include "StdAfx.h"
#include "CMyMutex.h"


// SHARED anti-collision mutex name, FIXED across every Hiss instance on this machine. The two phone
// bots mirror to the same desktop and share ONE mouse, so they MUST contend on the SAME named mutex --
// otherwise each bot locks its own private Preferences()->mutex_name() (which can differ per INI) and
// they click over each other, especially mid two-successive-clicks (Raise->Options) sequences. The
// holder keeps it across the WHOLE action (ExecutePrimaryFormulas + the atomic two-click HandleCycle),
// so the other bot(s) block here until the action completes. [Emrald: lock the other bot while one acts]
static const TCHAR *kHissSharedClickLock = _T("HissSharedMouseClickLock");

CMyMutex::CMyMutex() : _mutex(false, kHissSharedClickLock) {
  // We want a long timeout to let the instances act in FIFO order, but not forever (popups, table
  // timeout). FIFO is not guaranteed but works in practice.
	if (_mutex.Lock(5000)) {
    write_log(Preferences()->debug_autoplayer(), "[CMyMutex] successfully locked\n");
	  _locked = TRUE;
  }	else {
    write_log(Preferences()->debug_autoplayer(), "[CMyMutex] Timeout. Locking failed\n");
	  _locked = FALSE;
  }
}


CMyMutex::~CMyMutex(void) {
	if (_locked == TRUE) {   
    write_log(Preferences()->debug_autoplayer(), "[CMyMutex] Locked: %s\n", Bool2CString(_locked));  
		_mutex.Unlock();
  }
}

bool CMyMutex::IsLocked() {
	return _locked == TRUE;
}
