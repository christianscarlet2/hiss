//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include "stdafx.h"
#include "CrashHandler.h"

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdarg.h>
#include <signal.h>
#include <new.h>
#include <eh.h>
#include "..\DLLs\Files_DLL\Files.h"

#pragma comment(lib, "dbghelp.lib")

static char  g_tag[32] = "hiss";
static LONG  g_dump_seq = 0;
static volatile LONG g_in_handler = 0;   // guard against recursive crashes

static CString CrashDir() {
  // Reuse the logs directory the rest of Hiss writes to.
  return LogsDirectory();
}

static void CrashLog(const char *fmt, ...) {
  CString path = CrashDir();
  path.AppendFormat("crash_%s_%lu.log", g_tag, (unsigned long)GetCurrentProcessId());
  FILE *f = fopen(path.GetString(), "a");
  if (f == NULL) return;
  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fclose(f);
}

static void WriteMiniDump(EXCEPTION_POINTERS *ep) {
  CString path = CrashDir();
  path.AppendFormat("crash_%s_%lu_%ld.dmp", g_tag,
                    (unsigned long)GetCurrentProcessId(),
                    (long)InterlockedIncrement(&g_dump_seq));
  HANDLE hFile = CreateFile(path.GetString(), GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (hFile == INVALID_HANDLE_VALUE) return;
  MINIDUMP_EXCEPTION_INFORMATION mei;
  mei.ThreadId = GetCurrentThreadId();
  mei.ExceptionPointers = ep;
  mei.ClientPointers = FALSE;
  MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory |
                                       MiniDumpWithDataSegs |
                                       MiniDumpWithThreadInfo);
  MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, type,
                    ep ? &mei : NULL, NULL, NULL);
  CloseHandle(hFile);
  CrashLog("  minidump: %s\n", path.GetString());
}

// Walk the stack from the given context (or the current one) and log each frame
// with module + symbol + source line when Hiss.pdb is available.
static void LogStackTrace(CONTEXT *ctx_in) {
  HANDLE proc = GetCurrentProcess();
  HANDLE hThread = GetCurrentThread();
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
  SymInitialize(proc, NULL, TRUE);

  CONTEXT ctx;
  if (ctx_in != NULL) {
    ctx = *ctx_in;
  } else {
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    RtlCaptureContext(&ctx);
  }

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

  CrashLog("  call stack:\n");
  for (int depth = 0; depth < 64; ++depth) {
    if (!StackWalk64(machine, proc, hThread, &frame, &ctx, NULL,
                     SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
      break;
    }
    DWORD64 pc = frame.AddrPC.Offset;
    if (pc == 0) break;

    // Module name.
    char modname[MAX_PATH] = "?";
    IMAGEHLP_MODULE64 mod; ZeroMemory(&mod, sizeof(mod)); mod.SizeOfStruct = sizeof(mod);
    if (SymGetModuleInfo64(proc, pc, &mod)) {
      lstrcpynA(modname, mod.ModuleName, sizeof(modname));
    }

    // Symbol + displacement.
    char symbuf[sizeof(SYMBOL_INFO) + 512] = {0};
    SYMBOL_INFO *sym = (SYMBOL_INFO *)symbuf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 512;
    DWORD64 disp = 0;
    char symname[600];
    if (SymFromAddr(proc, pc, &disp, sym)) {
      _snprintf(symname, sizeof(symname), "%s+0x%llx", sym->Name, (unsigned long long)disp);
    } else {
      _snprintf(symname, sizeof(symname), "0x%llx", (unsigned long long)pc);
    }

    // Source line.
    IMAGEHLP_LINE64 line; ZeroMemory(&line, sizeof(line)); line.SizeOfStruct = sizeof(line);
    DWORD ldisp = 0;
    if (SymGetLineFromAddr64(proc, pc, &ldisp, &line)) {
      CrashLog("    [%02d] %s!%s  (%s:%lu)\n", depth, modname, symname,
               line.FileName, (unsigned long)line.LineNumber);
    } else {
      CrashLog("    [%02d] %s!%s\n", depth, modname, symname);
    }
  }
  SymCleanup(proc);
}

static void CrashHeader(const char *kind) {
  SYSTEMTIME st; GetLocalTime(&st);
  CrashLog("\n==== CRASH (%s) %04d-%02d-%02d %02d:%02d:%02d  pid=%lu tag=%s ====\n",
           kind, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
           (unsigned long)GetCurrentProcessId(), g_tag);
}

static LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS *ep) {
  if (InterlockedExchange(&g_in_handler, 1) != 0) return EXCEPTION_EXECUTE_HANDLER;
  CrashHeader("SEH");
  if (ep != NULL && ep->ExceptionRecord != NULL) {
    CrashLog("  exception code: 0x%08x  at 0x%p\n",
             ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
  }
  WriteMiniDump(ep);
  LogStackTrace(ep ? ep->ContextRecord : NULL);
  CrashLog("==== end crash ====\n");
  return EXCEPTION_EXECUTE_HANDLER;
}

static void OnTerminate() {
  if (InterlockedExchange(&g_in_handler, 1) != 0) { abort(); return; }
  CrashHeader("std::terminate (unhandled C++ exception)");
  WriteMiniDump(NULL);
  LogStackTrace(NULL);
  CrashLog("==== end crash ====\n");
  abort();
}

static void OnInvalidParameter(const wchar_t *, const wchar_t *,
                               const wchar_t *, unsigned int, uintptr_t) {
  if (InterlockedExchange(&g_in_handler, 1) != 0) return;
  CrashHeader("CRT invalid parameter");
  WriteMiniDump(NULL);
  LogStackTrace(NULL);
  CrashLog("==== end crash ====\n");
  g_in_handler = 0;  // let the CRT continue its default (which usually aborts)
}

static void OnPureCall() {
  if (InterlockedExchange(&g_in_handler, 1) != 0) return;
  CrashHeader("pure virtual call");
  WriteMiniDump(NULL);
  LogStackTrace(NULL);
  CrashLog("==== end crash ====\n");
}

static void OnSigAbort(int) {
  if (InterlockedExchange(&g_in_handler, 1) != 0) return;
  CrashHeader("SIGABRT / abort()");
  WriteMiniDump(NULL);
  LogStackTrace(NULL);
  CrashLog("==== end crash ====\n");
}

void InstallCrashHandler(const char *tag) {
  if (tag != NULL && *tag) lstrcpynA(g_tag, tag, sizeof(g_tag));
  SetUnhandledExceptionFilter(UnhandledFilter);
  set_terminate(OnTerminate);
  _set_invalid_parameter_handler(OnInvalidParameter);
  _set_purecall_handler(OnPureCall);
  signal(SIGABRT, OnSigAbort);
  // Make sure abort() doesn't pop the "Debug Error" message box and instead runs
  // our SIGABRT handler quietly.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}
