//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose: A small, self-contained (in-process) parallel worker pool shared by
//   Hiss, Vision and the trainer to offload bulk Tesseract OCR and image
//   hashing/detection onto several CPU threads. No external broker is required.
//
//   Generic task runner: callers Submit() std::function<void()> jobs and the pool
//   runs them on N worker threads. For "update as results arrive", a job should
//   compute its result and marshal it back to the UI thread (e.g. ::PostMessage),
//   not touch UI/GDI state directly.
//
//   Win32 threads are used on purpose (not std::thread): some of the MFC/PCH
//   include chains in this project turn the token `thread` into the TLS specifier,
//   which corrupts <thread>. CRITICAL_SECTION + CONDITION_VARIABLE avoid that.
//
//   THREAD-SAFETY for OCR/hash jobs:
//     * Tesseract's TessBaseAPI is NOT thread-safe; give each worker/job its own
//       instance, never share one across threads.
//     * A GDI HBITMAP can be selected into only one DC at a time; extract the
//       pixels a job needs into a private buffer/own bitmap before submitting.
//
//   Configured size is (num_cpus * workers_per_cpu), split across running app
//   instances; persisted in the shared hiss "settings" table key "parallel_workers".
//
//******************************************************************************
#pragma once

#include <windows.h>
#include <process.h>
#include <vector>
#include <queue>
#include <functional>

class ParallelWorkerPool {
 public:
  ParallelWorkerPool() : _sem(NULL), _stop(false), _started(false) {
    InitializeCriticalSection(&_cs);
  }
  ~ParallelWorkerPool() {
    Stop();
    DeleteCriticalSection(&_cs);
  }

  // (Re)start with the given number of worker threads (>=1). Safe to call again to
  // resize; pending jobs are dropped on resize.
  void Start(int threads) {
    Stop();
    if (threads < 1) threads = 1;
    _stop = false;
    _sem = CreateSemaphore(NULL, 0, 0x7fffffff, NULL);
    for (int i = 0; i < threads; ++i) {
      unsigned tid = 0;
      HANDLE h = (HANDLE)_beginthreadex(NULL, 0, &ParallelWorkerPool::ThreadEntry, this, 0, &tid);
      if (h != NULL) _workers.push_back(h);
    }
    _started = true;
  }

  void Stop() {
    if (_sem == NULL && _workers.empty()) return;
    EnterCriticalSection(&_cs);
    _stop = true;
    LeaveCriticalSection(&_cs);
    if (_sem != NULL) ReleaseSemaphore(_sem, (LONG)_workers.size() + 1, NULL);  // wake all
    for (size_t i = 0; i < _workers.size(); ++i) {
      if (_workers[i] != NULL) {
        WaitForSingleObject(_workers[i], INFINITE);
        CloseHandle(_workers[i]);
      }
    }
    _workers.clear();
    EnterCriticalSection(&_cs);
    std::queue<std::function<void()> > empty;
    std::swap(_jobs, empty);
    _stop = false;
    LeaveCriticalSection(&_cs);
    if (_sem != NULL) { CloseHandle(_sem); _sem = NULL; }
    _started = false;
  }

  void Submit(std::function<void()> job) {
    if (_sem == NULL) { if (job) job(); return; }   // not started: run inline
    EnterCriticalSection(&_cs);
    _jobs.push(job);
    LeaveCriticalSection(&_cs);
    ReleaseSemaphore(_sem, 1, NULL);
  }

  int ThreadCount() const { return (int)_workers.size(); }
  bool Started() const { return _started; }

 private:
  static unsigned __stdcall ThreadEntry(void *self) {
    ((ParallelWorkerPool *)self)->WorkerLoop();
    return 0;
  }
  void WorkerLoop() {
    for (;;) {
      WaitForSingleObject(_sem, INFINITE);
      std::function<void()> job;
      EnterCriticalSection(&_cs);
      if (_stop) { LeaveCriticalSection(&_cs); return; }
      if (!_jobs.empty()) { job = _jobs.front(); _jobs.pop(); }
      LeaveCriticalSection(&_cs);
      if (job) job();
    }
  }

  std::vector<HANDLE> _workers;
  std::queue<std::function<void()> > _jobs;
  CRITICAL_SECTION _cs;
  HANDLE _sem;
  bool _stop;
  bool _started;
};

// Logical CPU count (GetSystemInfo; avoids std::thread::hardware_concurrency).
inline int ParallelDefaultCpuCount() {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  int n = (int)si.dwNumberOfProcessors;
  return (n < 1) ? 1 : n;
}

// Total configured workers = num_cpus * workers_per_cpu (num_cpus<=0 => all CPUs).
inline int ParallelWorkerCount(int num_cpus, int workers_per_cpu) {
  if (num_cpus <= 0) num_cpus = ParallelDefaultCpuCount();
  if (workers_per_cpu <= 0) workers_per_cpu = 1;
  int n = num_cpus * workers_per_cpu;
  if (n < 1) n = 1;
  if (n > 256) n = 256;   // sanity cap
  return n;
}

// Instance-aware sizing: with several app instances open (e.g. multi-tabling Hiss),
// each in-process pool spawns its own threads, so split the configured total across
// the running instances to avoid CPU oversubscription (min 1 worker each).
inline int ParallelWorkerCountForInstances(int num_cpus, int workers_per_cpu, int instances) {
  int total = ParallelWorkerCount(num_cpus, workers_per_cpu);
  if (instances < 1) instances = 1;
  int per = total / instances;
  return (per < 1) ? 1 : per;
}
