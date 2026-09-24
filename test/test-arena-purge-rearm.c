/* Acceptance test for issue #457 criterion 1: the DEFERRED arena purge, with the process
   merely idling and no idle hook to help it.

   The sibling test `test-thread-idle-rss` covers the idle hook: its worker calls
   `mi_on_thread_idle()` and asserts the memory comes back. This test is the other half, and
   it deliberately does the opposite -- **nothing in this file calls `mi_on_thread_idle`**.
   Worker threads free their large blocks and then genuinely idle: no allocation, no idle
   hook, no explicit collect. The only thing that can give the memory back is the scavenger's
   deferred arena purge firing on the deadline `purge_delay` names.

   That path is what #457 is about. `subproc->purge_expire` -- the only deadline the
   scavenger waits on, and the only field `_mi_arenas_try_purge` checks on its opportunistic
   path -- is cleared to 0 by that function's settle CAS and by the scavenger's own
   stale-value CAS, but re-armed only on the 0 -> set transition of a *per-arena*
   `purge_expire`, which `mi_arena_try_purge` clears independently. When the settle CAS is
   skipped because a sweep could not claim every subproc (`all_visited` false), the two
   fields are left disagreeing, nothing re-arms them, and the scavenger parks on its 30 s
   safety net.

   WHY THIS TEST NEEDS MORE THAN ONE THREAD. That is the whole reason for NTHREADS: with a
   single parked worker a sweep *can* claim every subproc, `all_visited` is true, the settle
   CAS runs and rewrites the subproc deadline from the arena's, and the invariant is restored
   by accident. A single-threaded free-then-idle version of this test -- three rounds of
   192 MiB on one worker -- measured 5 MiB residual with the option OFF and 5 MiB with it ON,
   i.e. it did not exercise the defect at all. Concurrent churn is what makes a sweep's claim
   fail, and it is what the benchmark that found #457 did (8 workers).

   Preconditions are set by ctest, not here, so that this TU keeps compiling against a tree
   that has never heard of the option -- which is what lets the same binary serve as both the
   RED and the GREEN half of the demonstration:

       MIMALLOC_PURGE_REARM=1     the fix under test
       MIMALLOC_PURGE_DELAY=20    a deadline short enough to be observable in-process

   Public API only (mi_malloc / mi_free / mi_process_info), like test-thread-idle-rss, so it
   needs no internal header and no observability subsystem compiled in. */

#include <mimalloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* platform threads rather than <thread>/<chrono>: some mingw-w64 toolchains ship the win32
   threads model, where <thread> is not available at all (same reason as
   test-thread-idle-rss.cpp and test-stress.c). */
#if defined(_WIN32)
#include <windows.h>
typedef HANDLE mi_test_thread_t;
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t mi_test_thread_t;
static void sleep_ms(unsigned ms) { usleep(ms * 1000u); }
#endif

static size_t rss_bytes(void) {
  size_t elapsed = 0, user = 0, sys = 0, cur_rss = 0, peak_rss = 0, cur_commit = 0, peak_commit = 0, faults = 0;
  /* `current_rss`, never `peak_rss`: peak is ru_maxrss, which by construction never comes
     back down, so it could not observe a purge. */
  mi_process_info(&elapsed, &user, &sys, &cur_rss, &peak_rss, &cur_commit, &peak_commit, &faults);
  return cur_rss;
}

#define NTHREADS    (4)
#define BLOCK       (1u << 20)  /* 1 MiB -- well above MI_LARGE_MAX_OBJ_SIZE, so these take the
                                   large-object path that schedules a deferred arena purge */
#define BLOCKS      (48)        /* 48 MiB live per thread, 192 MiB across the process */
#define ROUNDS      (30)        /* free-all / allocate-all cycles per thread; the point is the
                                   number of schedule calls that race the sweeps they trigger */
#define POLL_MS     (5)
#define POLL_ITERS  (400)       /* 2 s of polling after the threads stop */

static volatile int    g_parks[NTHREADS];   /* each worker: 1 once it has parked */
static volatile int    g_release = 0;       /* main: 1 releases the parked workers */
static volatile int    g_failed = 0;

static void worker_body(int idx) {
  void** p = (void**)calloc(BLOCKS, sizeof(void*));
  if (p == NULL) { g_failed = 1; g_parks[idx] = 1; return; }

  for (int r = 0; r < ROUNDS; r++) {
    for (int i = 0; i < BLOCKS; i++) {
      p[i] = mi_malloc(BLOCK);
      if (p[i] == NULL) { g_failed = 1; free(p); g_parks[idx] = 1; return; }
      memset(p[i], (i + r) & 0xFF, BLOCK);   /* touch it, so it is really resident */
    }
    for (int i = 0; i < BLOCKS; i++) { mi_free(p[i]); }
    /* deliberately no mi_on_thread_idle() and no collect: the scavenger's deadline is the
       only thing that can act on these frees */
  }
  free(p);
  g_parks[idx] = 1;

  /* Stay alive through the measurement: if these threads exited, theap teardown would
     release the memory and the test would prove nothing about the scavenger. */
  while (!g_release) { sleep_ms(POLL_MS); }
}

#if defined(_WIN32)
static DWORD WINAPI worker_main(LPVOID a) { worker_body((int)(intptr_t)a); return 0; }
static void thread_start(mi_test_thread_t* t, int idx) {
  *t = CreateThread(NULL, 0, &worker_main, (LPVOID)(intptr_t)idx, 0, NULL);
}
static void thread_join(mi_test_thread_t t) { if (t != NULL) { WaitForSingleObject(t, INFINITE); CloseHandle(t); } }
#else
static void* worker_main(void* a) { worker_body((int)(intptr_t)a); return NULL; }
static void thread_start(mi_test_thread_t* t, int idx) {
  pthread_create(t, NULL, &worker_main, (void*)(intptr_t)idx);
}
static void thread_join(mi_test_thread_t t) { pthread_join(t, NULL); }
#endif

int main(void) {
  const size_t rss0 = rss_bytes();
  mi_test_thread_t t[NTHREADS];
  for (int i = 0; i < NTHREADS; i++) { g_parks[i] = 0; thread_start(&t[i], i); }

  /* Track the high-water RSS while the threads churn: that is what they held. main must not
     allocate in this loop either. */
  size_t live = rss0;
  for (;;) {
    const size_t now = rss_bytes();
    if (now > live) live = now;
    int parked = 0;
    for (int i = 0; i < NTHREADS; i++) { if (g_parks[i]) parked++; }
    if (parked == NTHREADS) break;
    sleep_ms(POLL_MS);
  }

  if (g_failed) {
    fprintf(stderr, "test-arena-purge-rearm: a worker could not allocate; skipping\n");
    g_release = 1;
    for (int i = 0; i < NTHREADS; i++) { thread_join(t[i]); }
    return 0;
  }

  const size_t held = (live > rss0 ? live - rss0 : 0);
  fprintf(stderr, "test-arena-purge-rearm: rss start %zu MiB, high water %zu MiB (held %zu MiB), "
                  "%d threads x %d rounds\n",
          rss0 / (1024 * 1024), live / (1024 * 1024), held / (1024 * 1024), NTHREADS, ROUNDS);

  if (held < 64u * 1024 * 1024) {
    fprintf(stderr, "test-arena-purge-rearm: only %zu MiB became resident; skipping (nothing to reclaim)\n",
            held / (1024 * 1024));
    g_release = 1;
    for (int i = 0; i < NTHREADS; i++) { thread_join(t[i]); }
    return 0;
  }

  /* Poll the WHOLE window without breaking early: the question is what the residual settles
     at, not how fast the first megabyte returns. An early exit at a weak threshold reports a
     value that is still falling, which makes the two configurations look alike. */
  size_t best = live;
  long best_ms = -1;
  for (int i = 0; i < POLL_ITERS; i++) {
    const size_t now = rss_bytes();
    if (now < best) { best = now; best_ms = (long)i * POLL_MS; }
    sleep_ms(POLL_MS);
  }
  const size_t dropped = (live > best ? live - best : 0);

  g_release = 1;
  for (int i = 0; i < NTHREADS; i++) { thread_join(t[i]); }

  fprintf(stderr, "test-arena-purge-rearm: rss after idle %zu MiB (dropped %zu MiB of %zu MiB held, "
                  "lowest at %ldms)\n",
          best / (1024 * 1024), dropped / (1024 * 1024), held / (1024 * 1024), best_ms);

  /* The claim is not "some memory comes back" -- with the deadline orphaned the first cycle's
     purge still runs and returns a fraction -- but that the deferred purge keeps running, so
     the residual settles near the process's starting footprint instead of stranding a large
     share of the freed bytes. */
  if (best > rss0 + held / 8) {
    fprintf(stderr, "test-arena-purge-rearm: FAILED -- %zu MiB of the %zu MiB freed is still resident "
                    "after %dms of idling. Without `mi_option_purge_rearm` the scavenger's deadline is "
                    "left orphaned once a sweep cannot claim every subproc, so the deferred purge stops "
                    "running and `purge_delay` stops doing anything (#457).\n",
            (best > rss0 ? best - rss0 : 0) / (1024 * 1024), held / (1024 * 1024), POLL_ITERS * POLL_MS);
    return 1;
  }
  fprintf(stderr, "test-arena-purge-rearm: ok\n");
  return 0;
}
