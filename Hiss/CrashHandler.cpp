//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Hardened crash handler.
//
//   Design goals (learned from truncated crash logs that captured only the
//   exception line):
//     * NO heap / CRT use inside the handler. A heap-corruption crash makes
//       malloc/fopen fault re-entrantly, so we cache the log path at install
//       time and write with raw Win32 (CreateFile/WriteFile/FlushFileBuffers),
//       flushing every line so whatever we manage to write always survives.
//     * Run the dangerous work (MiniDumpWriteDump + StackWalk) on a SEPARATE
//       worker thread with a fresh stack, so a STACK OVERFLOW in the faulting
//       thread doesn't deny the handler the stack it needs.
//     * SEH-guard the dump and the stack walk independently, so a fault inside
//       one still lets the other (and the "end crash" marker) be written.
//     * Reserve a stack guarantee on installing threads so the SEH filter has
//       room to even reach the point of spawning the worker.
//
//******************************************************************************

#include "stdafx.h"
#include "CrashHandler.h"

#include <windows.h>
#include <dbghelp.h>
#include <stdarg.h>
#include <signal.h>
#include <new.h>
#include <eh.h>
#include "..\DLLs\Files_DLL\Files.h"

#pragma comment(lib, "dbghelp.lib")

static char  g_tag[32] = "hiss";
static char  g_log_path[MAX_PATH] = {0};   // cached at install (no heap in handler)
static char  g_dump_dir[MAX_PATH] = {0};   // logs directory, trailing backslash
static LONG  g_dump_seq = 0;
static volatile LONG g_in_handler = 0;     // guard against recursive crashes

// ---- Raw, allocation-free logging -------------------------------------------
// Append bytes to the crash log and flush immediately. Opens/closes per call so
// a later fault cannot lose already-written, flushed data.
static void RawWrite(const char *s, DWORD len) {
  if (g_log_path[0] == 0 || s == NULL || len == 0) return;
  HANDLE h = CreateFileA(g_log_path, FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return;
  SetFilePointer(h, 0, NULL, FILE_END);
  DWORD wrote = 0;
  WriteFile(h, s, len, &wrote, NULL);
  FlushFileBuffers(h);
  CloseHandle(h);
}

// wvsprintfA lives in user32 and does NOT touch the CRT heap. It supports
// %s %d %u %x %X %c %ld %lu %lx %p and width/zero padding -- enough for us.
// It does NOT support %llx / %f, so 64-bit addresses are formatted by hand.
static void LogF(const char *fmt, ...) {
  char buf[1024];
  va_list a;
  va_start(a, fmt);
  wvsprintfA(buf, fmt, a);
  va_end(a);
  RawWrite(buf, (DWORD)lstrlenA(buf));
}

// Format a 64-bit address as hex into caller's buffer (no heap).
static void Hex64(char *out, DWORD64 v) {
  DWORD hi = (DWORD)(v >> 32), lo = (DWORD)(v & 0xFFFFFFFFu);
  if (hi != 0) wsprintfA(out, "%lX%08lX", hi, lo);
  else         wsprintfA(out, "%lX", lo);
}

// ---- Minidump (SEH-guarded, runs on the worker thread) ----------------------
static void GuardedWriteMiniDump(EXCEPTION_POINTERS *ep, DWORD faulting_tid) {
  char path[MAX_PATH];
  wsprintfA(path, "%scrash_%s_%lu_%ld.dmp", g_dump_dir, g_tag,
            (unsigned long)GetCurrentProcessId(),
            (long)InterlockedIncrement(&g_dump_seq));
  LogF("  writing minidump: %s\n", path);   // marker BEFORE (so we see if it faults)
  __try {
    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
      LogF("  minidump: CreateFile failed (err %lu)\n", GetLastError());
      return;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = faulting_tid;
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    // Conservative dump type: enough to symbolicate stacks WITHOUT the risky
    // MiniDumpWithIndirectlyReferencedMemory pass (that walks heap pointers and
    // can itself fault during a heap-corruption crash).
    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithThreadInfo |
                                         MiniDumpWithDataSegs);
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                hFile, type, ep ? &mei : NULL, NULL, NULL);
    CloseHandle(hFile);
    LogF(ok ? "  minidump: ok\n" : "  minidump: MiniDumpWriteDump failed\n");
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogF("  minidump: FAULTED while writing (skipped)\n");
  }
}

// ---- Stack walk (SEH-guarded, runs on the worker thread) --------------------
// Walks the FAULTING thread's stack using its captured context + thread handle.
static void GuardedLogStackTrace(CONTEXT *ctx_in, HANDLE faulting_thread) {
  __try {
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, NULL, TRUE);

    CONTEXT ctx = *ctx_in;
    STACKFRAME64 frame;
    ZeroMemory(&frame, sizeof(frame));
#ifdef _M_IX86
    DWORD machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = ctx.Eip;       frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Ebp;    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Esp;    frame.AddrStack.Mode = AddrModeFlat;
#else
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctx.Rip;       frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;    frame.AddrStack.Mode = AddrModeFlat;
#endif

    LogF("  call stack:\n");
    for (int depth = 0; depth < 64; ++depth) {
      if (!StackWalk64(machine, proc, faulting_thread, &frame, &ctx, NULL,
                       SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
        break;
      }
      DWORD64 pc = frame.AddrPC.Offset;
      if (pc == 0) break;

      char modname[MAX_PATH] = "?";
      IMAGEHLP_MODULE64 mod; ZeroMemory(&mod, sizeof(mod)); mod.SizeOfStruct = sizeof(mod);
      if (SymGetModuleInfo64(proc, pc, &mod)) {
        lstrcpynA(modname, mod.ModuleName, sizeof(modname));
      }

      char symbuf[sizeof(SYMBOL_INFO) + 512] = {0};
      SYMBOL_INFO *sym = (SYMBOL_INFO *)symbuf;
      sym->SizeOfStruct = sizeof(SYMBOL_INFO);
      sym->MaxNameLen = 512;
      DWORD64 disp = 0;
      char pchex[24]; Hex64(pchex, pc);
      IMAGEHLP_LINE64 line; ZeroMemory(&line, sizeof(line)); line.SizeOfStruct = sizeof(line);
      DWORD ldisp = 0;
      bool have_line = (SymGetLineFromAddr64(proc, pc, &ldisp, &line) != FALSE);

      if (SymFromAddr(proc, pc, &disp, sym)) {
        char dhex[24]; Hex64(dhex, disp);
        if (have_line) {
          LogF("    [%02d] %s!%s+0x%s  (%s:%lu)\n", depth, modname, sym->Name, dhex,
               line.FileName, (unsigned long)line.LineNumber);
        } else {
          LogF("    [%02d] %s!%s+0x%s\n", depth, modname, sym->Name, dhex);
        }
      } else {
        LogF("    [%02d] %s!0x%s\n", depth, modname, pchex);
      }
    }
    SymCleanup(proc);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogF("  call stack: FAULTED during walk (partial above)\n");
  }
}

// ---- Worker thread: fresh stack for the dump + walk -------------------------
struct CrashWork {
  EXCEPTION_POINTERS *ep;
  CONTEXT             ctx;          // captured faulting-thread context
  HANDLE              faulting_thread;
  DWORD               faulting_tid;
};

static DWORD WINAPI CrashWorkerThread(LPVOID p) {
  CrashWork *w = (CrashWork *)p;
  GuardedWriteMiniDump(w->ep, w->faulting_tid);
  GuardedLogStackTrace(&w->ctx, w->faulting_thread);
  return 0;
}

// Run the capture on a separate thread (fresh stack -> survives stack overflow
// of the faulting thread). Falls back to inline if the thread can't be created.
static void RunCrashCapture(EXCEPTION_POINTERS *ep) {
  CrashWork work;
  ZeroMemory(&work, sizeof(work));
  work.ep = ep;
  work.faulting_tid = GetCurrentThreadId();
  // A real handle to the faulting (current) thread, usable from the worker.
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                  &work.faulting_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
  if (ep != NULL && ep->ContextRecord != NULL) {
    work.ctx = *ep->ContextRecord;
  } else {
    work.ctx.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&work.ctx);   // best effort for non-SEH paths
  }

  HANDLE hWorker = CreateThread(NULL, 0, CrashWorkerThread, &work, 0, NULL);
  if (hWorker != NULL) {
    // Bounded wait so a wedged dump can't hang the dying process forever.
    WaitForSingleObject(hWorker, 20000);
    CloseHandle(hWorker);
  } else {
    // Couldn't spawn a thread (rare): do it inline as a last resort.
    GuardedWriteMiniDump(work.ep, work.faulting_tid);
    GuardedLogStackTrace(&work.ctx, work.faulting_thread);
  }
  if (work.faulting_thread) CloseHandle(work.faulting_thread);
}

static void CrashHeader(const char *kind) {
  SYSTEMTIME st; GetLocalTime(&st);
  LogF("\n==== CRASH (%s) %04d-%02d-%02d %02d:%02d:%02d  pid=%lu tag=%s ====\n",
       kind, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
       (unsigned long)GetCurrentProcessId(), g_tag);
}

// Common body for the non-SEH handlers (terminate / invalid-param / etc.).
static void HandleNonSeh(const char *kind) {
  if (InterlockedExchange(&g_in_handler, 1) != 0) return;
  CrashHeader(kind);
  RunCrashCapture(NULL);
  LogF("==== end crash ====\n");
}

static LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS *ep) {
  if (InterlockedExchange(&g_in_handler, 1) != 0) return EXCEPTION_EXECUTE_HANDLER;
  CrashHeader("SEH");
  if (ep != NULL && ep->ExceptionRecord != NULL) {
    char ahex[24]; Hex64(ahex, (DWORD64)(uintptr_t)ep->ExceptionRecord->ExceptionAddress);
    LogF("  exception code: 0x%08lx  at 0x%s\n",
         (unsigned long)ep->ExceptionRecord->ExceptionCode, ahex);
  }
  RunCrashCapture(ep);
  LogF("==== end crash ====\n");
  return EXCEPTION_EXECUTE_HANDLER;
}

static void OnTerminate() {
  HandleNonSeh("std::terminate (unhandled C++ exception)");
  abort();
}

static void OnInvalidParameter(const wchar_t *, const wchar_t *,
                               const wchar_t *, unsigned int, uintptr_t) {
  HandleNonSeh("CRT invalid parameter");
  g_in_handler = 0;  // let the CRT continue its default (which usually aborts)
}

static void OnPureCall() {
  HandleNonSeh("pure virtual call");
}

static void OnSigAbort(int) {
  HandleNonSeh("SIGABRT / abort()");
}

void InstallCrashHandler(const char *tag) {
  if (tag != NULL && *tag) lstrcpynA(g_tag, tag, sizeof(g_tag));

  // Cache the logs directory + crash-log path ONCE, here, so the handler never
  // has to allocate (CString / fopen) while the heap may be corrupt.
  CString dir = LogsDirectory();
  lstrcpynA(g_dump_dir, dir.GetString(), sizeof(g_dump_dir));
  size_t n = lstrlenA(g_dump_dir);
  if (n == 0 || (g_dump_dir[n - 1] != '\\' && g_dump_dir[n - 1] != '/')) {
    if (n + 1 < sizeof(g_dump_dir)) { g_dump_dir[n] = '\\'; g_dump_dir[n + 1] = 0; }
  }
  wsprintfA(g_log_path, "%scrash_%s_%lu.log", g_dump_dir, g_tag,
            (unsigned long)GetCurrentProcessId());

  // Reserve stack so the SEH filter can run even on a stack-overflow crash long
  // enough to spawn the fresh-stack worker thread.
  ULONG guarantee = 64 * 1024;
  SetThreadStackGuarantee(&guarantee);

  SetUnhandledExceptionFilter(UnhandledFilter);
  set_terminate(OnTerminate);
  _set_invalid_parameter_handler(OnInvalidParameter);
  _set_purecall_handler(OnPureCall);
  signal(SIGABRT, OnSigAbort);
  // Make sure abort() doesn't pop the "Debug Error" message box and instead runs
  // our SIGABRT handler quietly.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
