//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************

#include "stdafx.h"
#include "COcrWorker.h"

#include "CAutoOcr.h"
#include "..\CTablemap\CTablemap.h"
#include "..\CTablemap\CTablemapDB.h"
#include "..\DLLs\Files_DLL\Files.h"

// Set true in InitInstance when "--ocr-worker" is present (before singletons are
// built). Consulted by CSharedMem so workers stay out of the shared PID table.
bool g_ocr_worker_mode = false;

// Wire protocol over the duplex byte pipe (length-prefixed, little-endian):
//   request : u32 region_name_len, name bytes, i32 width, i32 height,
//             width*height*4 BGRA bytes
//   reply   : u32 text_len, text bytes (UTF-8)
// A zero region_name_len is a shutdown sentinel.

static const DWORD kMaxNameLen   = 256;
static const DWORD kMaxTextLen   = 1u << 20;          // 1 MB of text is absurd; cap
static const DWORD kMaxImageBytes = 64u * 1024 * 1024; // 64 MB region image cap

static bool ReadAll(HANDLE h, void* buf, DWORD n) {
  BYTE* p = (BYTE*)buf;
  DWORD done = 0;
  while (done < n) {
    DWORD got = 0;
    if (!ReadFile(h, p + done, n - done, &got, NULL) || got == 0) return false;
    done += got;
  }
  return true;
}

static bool WriteAll(HANDLE h, const void* buf, DWORD n) {
  const BYTE* p = (const BYTE*)buf;
  DWORD done = 0;
  while (done < n) {
    DWORD put = 0;
    if (!WriteFile(h, p + done, n - done, &put, NULL) || put == 0) return false;
    done += put;
  }
  return true;
}

//==============================================================================
//  Command line
//==============================================================================

static CString ExtractFlagValue(const CString& cmd, const CString& flag) {
  int p = cmd.Find(flag);
  if (p < 0) return CString("");
  int start = p + flag.GetLength();
  // Value runs to the next space, unless quoted.
  CString rest = cmd.Mid(start);
  rest.TrimLeft();
  if (!rest.IsEmpty() && rest[0] == '"') {
    int end = rest.Find('"', 1);
    if (end > 0) return rest.Mid(1, end - 1);
  }
  int sp = rest.Find(' ');
  if (sp < 0) return rest;
  return rest.Left(sp);
}

bool ParseOcrWorkerCommandLine(CString* pipe_name, CString* tablemap_name) {
  CString cmd = ::GetCommandLine();
  if (cmd.Find("--ocr-worker") < 0) return false;
  if (pipe_name)     *pipe_name     = ExtractFlagValue(cmd, "--pipe=");
  if (tablemap_name) *tablemap_name = ExtractFlagValue(cmd, "--tablemap=");
  return true;
}

//==============================================================================
//  Worker side
//==============================================================================

void RunOcrWorker(const CString& pipe_name, const CString& tablemap_name) {
 try {
  // Load the same tablemap + OCR settings the coordinator is using, then service
  // recognition requests until the pipe closes. Everything here runs in this
  // process's own heap, isolated from the main Hiss.
  if (p_tablemap_db != NULL && p_tablemap != NULL && !tablemap_name.IsEmpty()) {
    p_tablemap_db->LoadTablemapFromDB(tablemap_name, p_tablemap);
  }
  AutoOcr()->LoadModelSettings();

  HANDLE pipe = CreateFile(pipe_name.GetString(), GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
  if (pipe == INVALID_HANDLE_VALUE) {
    ExitProcess(2);
  }

  std::vector<unsigned char> img;
  for (;;) {
    DWORD namelen = 0;
    if (!ReadAll(pipe, &namelen, sizeof(namelen))) break;
    if (namelen == 0 || namelen > kMaxNameLen) break;   // shutdown / bad frame
    char namebuf[kMaxNameLen + 1] = {0};
    if (!ReadAll(pipe, namebuf, namelen)) break;
    namebuf[namelen] = 0;
    INT32 w = 0, h = 0;
    if (!ReadAll(pipe, &w, sizeof(w)) || !ReadAll(pipe, &h, sizeof(h))) break;
    if (w <= 0 || h <= 0) break;
    DWORD nbytes = (DWORD)w * (DWORD)h * 4u;
    if (nbytes == 0 || nbytes > kMaxImageBytes) break;
    img.resize(nbytes);
    if (!ReadAll(pipe, img.data(), nbytes)) break;

    CString text;
    if (p_tablemap != NULL) {
      RMapCI it = p_tablemap->r$()->find(namebuf);
      if (it != p_tablemap->r$()->end()) {
        try {
          Mat m(h, w, CV_8UC4, img.data());
          text = AutoOcr()->get_ocr_result(m.clone(), it);
        } catch (...) {
          text = "";
        }
      }
    }
    CStringA t(text);
    DWORD tlen = (DWORD)t.GetLength();
    if (!WriteAll(pipe, &tlen, sizeof(tlen))) break;
    if (tlen && !WriteAll(pipe, t.GetString(), tlen)) break;
  }
  CloseHandle(pipe);
 } catch (...) {
  // A worker must never propagate a C++ exception out of here (that would be a
  // 0xC0000409 crash). It's isolated anyway, but exit cleanly so the coordinator
  // simply respawns it and the affected region falls back to its last OCR value.
 }
  // Skip MFC/singleton teardown (it asserts threads were started); just exit.
  ExitProcess(0);
}

//==============================================================================
//  Coordinator side
//==============================================================================

COcrWorkerPool::COcrWorkerPool() : _size(0), _job(NULL) {}
COcrWorkerPool::~COcrWorkerPool() {
  Stop();
  if (_job) { CloseHandle(_job); _job = NULL; }   // closing the job kills survivors
}

void COcrWorkerPool::EnsureJob() {
  if (_job != NULL) return;
  _job = CreateJobObject(NULL, NULL);
  if (_job == NULL) return;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
  ZeroMemory(&info, sizeof(info));
  // Kill every assigned worker when the last handle to the job closes -- which
  // happens automatically when the main Hiss process exits or crashes.
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(_job, JobObjectExtendedLimitInformation, &info, sizeof(info));
}

void COcrWorkerPool::StartOne(int index) {
  CString pname;
  pname.Format("\\\\.\\pipe\\hiss_ocr_%lu_%d", (unsigned long)GetCurrentProcessId(), index);
  HANDLE pipe = CreateNamedPipe(pname.GetString(), PIPE_ACCESS_DUPLEX,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                1, 1u << 20, 1u << 20, 0, NULL);
  if (pipe == INVALID_HANDLE_VALUE) { _pipes[index] = NULL; _procs[index] = NULL; return; }

  char exe[MAX_PATH] = {0};
  GetModuleFileNameA(NULL, exe, MAX_PATH);
  CString cmd;
  cmd.Format("\"%s\" --ocr-worker --pipe=%s --tablemap=\"%s\"",
             exe, pname.GetString(), _tablemap_name.GetString());
  std::vector<char> buf(cmd.GetString(), cmd.GetString() + cmd.GetLength());
  buf.push_back(0);

  EnsureJob();
  STARTUPINFOA si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
  PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
  // Create suspended so we can assign it to the kill-on-close job BEFORE it runs
  // (no window where a crash could orphan it).
  if (!CreateProcessA(NULL, buf.data(), NULL, NULL, FALSE,
                      CREATE_NO_WINDOW | CREATE_SUSPENDED,
                      NULL, NULL, &si, &pi)) {
    CloseHandle(pipe);
    _pipes[index] = NULL; _procs[index] = NULL;
    return;
  }
  if (_job) AssignProcessToJobObject(_job, pi.hProcess);
  ResumeThread(pi.hThread);
  if (pi.hThread) CloseHandle(pi.hThread);
  // Wait for the worker to connect (it loads the tablemap first, so allow time).
  BOOL connected = ConnectNamedPipe(pipe, NULL);
  if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
    CloseHandle(pipe);
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    _pipes[index] = NULL; _procs[index] = NULL;
    return;
  }
  _pipes[index] = pipe;
  _procs[index] = pi.hProcess;
  write_log(k_always_log_basic_information, "[OcrWorker] worker %d started\n", index);
}

void COcrWorkerPool::StopOne(int index) {
  if (index < 0 || index >= (int)_pipes.size()) return;
  if (_pipes[index] != NULL) {
    // Send the shutdown sentinel (0 name-len), then close.
    DWORD zero = 0; WriteAll(_pipes[index], &zero, sizeof(zero));
    CloseHandle(_pipes[index]);
    _pipes[index] = NULL;
  }
  if (_procs[index] != NULL) {
    if (WaitForSingleObject(_procs[index], 1500) == WAIT_TIMEOUT) {
      TerminateProcess(_procs[index], 1);
    }
    CloseHandle(_procs[index]);
    _procs[index] = NULL;
  }
}

void COcrWorkerPool::Stop() {
  for (int i = 0; i < (int)_pipes.size(); ++i) StopOne(i);
  _pipes.clear();
  _procs.clear();
  _size = 0;
  _tablemap_name = "";
}

void COcrWorkerPool::EnsureStarted(int n, const CString& tablemap_name) {
  if (n < 1) n = 1;
  // Rebuild from scratch if the pool size or the tablemap changed.
  if (n != _size || tablemap_name != _tablemap_name) {
    Stop();
    _tablemap_name = tablemap_name;
    _size = n;
    _pipes.assign(n, NULL);
    _procs.assign(n, NULL);
    for (int i = 0; i < n; ++i) StartOne(i);
    return;
  }
  // Same config: respawn any worker that has died.
  for (int i = 0; i < _size; ++i) {
    bool dead = (_pipes[i] == NULL) || (_procs[i] == NULL)
              || (WaitForSingleObject(_procs[i], 0) == WAIT_OBJECT_0);
    if (dead) {
      StopOne(i);
      StartOne(i);
    }
  }
}

std::vector<HANDLE> COcrWorkerPool::Pipes() {
  std::vector<HANDLE> out;
  for (int i = 0; i < (int)_pipes.size(); ++i) {
    if (_pipes[i] != NULL) out.push_back(_pipes[i]);
  }
  return out;
}

bool COcrWorkerPool::Recognize(HANDLE pipe, const char* region_name,
                               const unsigned char* bgra, int width, int height,
                               CString* out_text) {
  if (pipe == NULL || region_name == NULL || bgra == NULL) return false;
  DWORD namelen = (DWORD)strlen(region_name);
  if (namelen == 0 || namelen > kMaxNameLen) return false;
  INT32 w = width, h = height;
  DWORD nbytes = (DWORD)width * (DWORD)height * 4u;
  if (!WriteAll(pipe, &namelen, sizeof(namelen))) return false;
  if (!WriteAll(pipe, region_name, namelen)) return false;
  if (!WriteAll(pipe, &w, sizeof(w))) return false;
  if (!WriteAll(pipe, &h, sizeof(h))) return false;
  if (!WriteAll(pipe, bgra, nbytes)) return false;
  DWORD tlen = 0;
  if (!ReadAll(pipe, &tlen, sizeof(tlen))) return false;
  if (tlen > kMaxTextLen) return false;
  std::vector<char> tb(tlen + 1, 0);
  if (tlen && !ReadAll(pipe, tb.data(), tlen)) return false;
  if (out_text) *out_text = CString(tb.data());
  return true;
}
