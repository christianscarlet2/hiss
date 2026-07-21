//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: Process-level OCR parallelism.
//
//   In-process multithreaded Tesseract corrupts the heap (leptonica global
//   state is not thread-safe in this build -> 0xC0000374). Instead of running
//   recognition on worker THREADS that share Hiss's heap, we run it in separate
//   WORKER PROCESSES: each is a "Hiss.exe --ocr-worker" that owns its own
//   Tesseract + leptonica + heap, so a crash or corruption can never reach the
//   main Hiss. Hiss is the coordinator: it captures region pixels (GDI, serial),
//   ships them over a per-worker named pipe, and collects the recognized text
//   into the same _ocr_cache the in-process pre-pass used.
//
//******************************************************************************

#ifndef INC_COCRWORKER_H
#define INC_COCRWORKER_H

#include <windows.h>
#include <vector>

// ---- Worker side (own process) ----
// Run the OCR worker loop: load the named tablemap from the DB, connect to the
// coordinator's pipe, then {read region+image -> OCR -> write text} until the
// pipe closes. Called from InitInstance when "--ocr-worker" is present. Never
// returns to normal startup (it ExitProcess()es when the pipe ends).
void RunOcrWorker(const CString& pipe_name, const CString& tablemap_name);

// Parse "--ocr-worker --pipe=<name> --tablemap=<name>" out of the command line.
// Returns true if this process should run as an OCR worker.
bool ParseOcrWorkerCommandLine(CString* pipe_name, CString* tablemap_name);

// True once InitInstance has determined this process is an OCR worker. Worker
// processes must stay out of the cross-instance machinery (session counter,
// shared-memory PID table / watchdog) so they neither exhaust the session
// counter nor get killed by another instance's watchdog. Set before
// InstantiateAllSingletons() so the singletons can consult it.
extern bool g_ocr_worker_mode;

// True if `pid` is an OCR worker THIS process spawned. Shutdown must not identify workers by
// descendancy: COpenHoldemStarter launches additional BOT instances as our children too, and
// treating those as workers is what made closing one table kill the others.
bool IsOwnOcrWorkerPid(DWORD pid);

// ---- Coordinator side (main Hiss) ----
// A warm pool of worker processes, one duplex named pipe each. PreOcrParallel
// borrows pipe handles (one outstanding request per pipe) to OCR regions
// out-of-process. Dead workers are detected and respawned.
class COcrWorkerPool {
 public:
  COcrWorkerPool();
  ~COcrWorkerPool();
  // Ensure `n` live workers exist for `tablemap_name`; (re)spawns as needed.
  // Rebuilds from scratch if the requested size or tablemap changed.
  void EnsureStarted(int n, const CString& tablemap_name);
  void Stop();
  // Number of workers this pool intends to run (the worker Hiss.exe processes we
  // spawned). Used to exclude our own workers from the Hiss-instance count.
  int Size() const { return _size; }
  // Snapshot of the currently-live worker pipe handles (for borrow/return).
  std::vector<HANDLE> Pipes();
  // Send one BGRA region image to a worker pipe and read back the recognized
  // text. Returns false on any pipe failure (caller should treat as "no result"
  // so the region falls back to its previous OCR via change-detection).
  static bool Recognize(HANDLE pipe, const char* region_name,
                        const unsigned char* bgra, int width, int height,
                        CString* out_text);
 private:
  void StartOne(int index);
  void StopOne(int index);
  void EnsureJob();             // lazily create the kill-on-close job object
  std::vector<HANDLE> _pipes;   // duplex pipe to each worker (server end)
  std::vector<HANDLE> _procs;   // worker process handles
  CString _tablemap_name;
  int _size;
  // All workers are assigned to this job; it is created with
  // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE so that when the main Hiss process dies --
  // even by a hard crash -- the OS terminates every worker. This is what prevents
  // orphaned "Hiss.exe --ocr-worker" processes from piling up across crashes.
  HANDLE _job;
};

#endif  // INC_COCRWORKER_H
