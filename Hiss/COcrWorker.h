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
  std::vector<HANDLE> _pipes;   // duplex pipe to each worker (server end)
  std::vector<HANDLE> _procs;   // worker process handles
  CString _tablemap_name;
  int _size;
};

#endif  // INC_COCRWORKER_H
