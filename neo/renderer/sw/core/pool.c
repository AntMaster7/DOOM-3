/* core/pool.c -- the thread pool. Lifted from CRenderer's core/pool.c; its job switch became a
   function pointer, and shutdown is real because the engine restarts its renderer (vid_restart).

   One pool, reused by every phase. dispatch() sets the job function and count, wakes the
   workers, and runs jobs on the calling thread too; workers race for jobs on a single atomic
   counter, so a slow job never strands a core. The caller is the engine's main thread; workers
   never call into the engine (I7).

   Lessons carried over (CRenderer CLAUDE.md): spinning instead of blocking measured slower (the
   pause loops steal boost clocks from real work); an empty dispatch costs 0.02 - 0.06 ms. */
#include "base.h"
#include "config.h"

typedef void (*SwJobFn)(int job, int tid);

static int g_numWorkers;                /* worker threads, excluding the caller */
static int g_numThreads;                /* workers + caller */
static HANDLE g_sem, g_doneEvent;       /* wake the workers / the phase is finished */
static HANDLE g_workerHandles[MAX_THREADS];
static volatile LONG g_quitWorkers;

/* the counter every thread hammers gets a cache line to itself (see g_stats in core/types.h) */
static __declspec(align(64)) volatile LONG g_jobNext;
static __declspec(align(64)) volatile LONG g_activeThreads;
static __declspec(align(64)) volatile LONG g_jobTotal;
static SwJobFn volatile g_jobFn;

static void run_jobs(int tid)
{
    const SwJobFn fn = g_jobFn;
    for (;;) {
        LONG j = InterlockedIncrement(&g_jobNext) - 1;   /* claim the next job index */
        if (j >= g_jobTotal) break;
        fn((int)j, tid);
    }
    /* the last thread out of the phase signals completion */
    if (InterlockedDecrement(&g_activeThreads) == 0) SetEvent(g_doneEvent);
}

static DWORD WINAPI worker_proc(LPVOID param)
{
    int tid = (int)(intptr_t)param;              /* 1..g_numWorkers; the caller is 0 */
    for (;;) {
        WaitForSingleObject(g_sem, INFINITE);
        if (g_quitWorkers) return 0;
        run_jobs(tid);
    }
}

/* Run a phase to completion, calling thread included. */
static void dispatch(SwJobFn fn, int total)
{
    if (total <= 0) return;
    g_jobFn = fn;
    g_jobTotal = total;
    g_jobNext = 0;
    g_activeThreads = g_numThreads;
    if (g_numWorkers) ReleaseSemaphore(g_sem, g_numWorkers, NULL);
    run_jobs(0);
    WaitForSingleObject(g_doneEvent, INFINITE);
}

static void pool_init(int threads)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    g_numThreads = threads > 0 ? threads : (int)si.dwNumberOfProcessors;
    if (g_numThreads < 1) g_numThreads = 1;
    if (g_numThreads > MAX_THREADS) g_numThreads = MAX_THREADS;
    g_numWorkers = g_numThreads - 1;
    g_quitWorkers = 0;
    g_sem = CreateSemaphoreA(NULL, 0, 0x7FFFFFFF, NULL);    /* one release per worker per phase */
    g_doneEvent = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset: one waiter per phase */
    for (int i = 0; i < g_numWorkers; i++)
        g_workerHandles[i] = CreateThread(NULL, 0, worker_proc, (LPVOID)(intptr_t)(i + 1), 0, NULL);
}

static void pool_shutdown(void)
{
    if (!g_sem) return;
    g_quitWorkers = 1;
    if (g_numWorkers) {
        ReleaseSemaphore(g_sem, g_numWorkers, NULL);
        WaitForMultipleObjects((DWORD)g_numWorkers, g_workerHandles, TRUE, INFINITE);
        for (int i = 0; i < g_numWorkers; i++) CloseHandle(g_workerHandles[i]);
    }
    CloseHandle(g_sem); CloseHandle(g_doneEvent);
    g_sem = g_doneEvent = NULL;
    g_numWorkers = g_numThreads = 0;
}
