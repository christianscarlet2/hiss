//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Always-on crash interception ("debugging mode").
//
//   Installs handlers for every way Hiss has been dying:
//     * SEH crashes  (access violation 0xC0000005, heap corruption 0xC0000374)
//     * unhandled C++ exceptions -> std::terminate (0xC0000409)
//     * CRT invalid-parameter / pure-call / abort()
//   On any of them it writes, to logs\crash_<pid>.log :
//     - the exception code + faulting address
//     - a SYMBOLICATED call stack (needs Hiss.pdb next to Hiss.exe)
//   and a full minidump  logs\crash_<pid>_<seq>.dmp  for post-mortem in WinDbg/VS.
//
//   Call InstallCrashHandler() as early as possible (also in OCR-worker mode).
//
//******************************************************************************

#ifndef INC_CRASHHANDLER_H
#define INC_CRASHHANDLER_H

// tag is written into the log/dump filename so main-Hiss vs ocr-worker crashes
// are distinguishable (e.g. "hiss", "ocrworker").
void InstallCrashHandler(const char *tag);

#endif  // INC_CRASHHANDLER_H
