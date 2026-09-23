/* Standalone measurement child for ci/bench_arena_reclaim.py (#438).
   No CMake test registration: this is a benchmark, not a correctness gate. */
#include <mimalloc.h>
#include <mimalloc-stats.h>
#ifdef __cplusplus
#include <atomic>
#ifndef MI_USE_CXX
extern "C" {
#endif
#endif
#include "mimalloc/internal.h"
#ifdef __cplusplus
#ifndef MI_USE_CXX
}
#endif
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <time.h>
#endif

#define BIG_BYTES ((size_t)32 * 1024 * 1024)
#define BIG_COUNT 8

static double seconds_now(void) {
#ifdef _WIN32
  LARGE_INTEGER frequency, counter;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&counter);
  return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

typedef struct process_memory_s {
  uint64_t rss, private_bytes, virtual_bytes;
} process_memory_t;

static process_memory_t process_memory(void) {
  process_memory_t m = {0, 0, 0};
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX pm;
  memset(&pm, 0, sizeof(pm));
  pm.cb = sizeof(pm);
  if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm))) exit(3);
  m.rss = (uint64_t)pm.WorkingSetSize;
  m.private_bytes = (uint64_t)pm.PrivateUsage;
  /* Windows' process counters do not expose Linux VmSize's equivalent. The
     allocator reservation gauge below is separate, never substituted here. */
#else
  FILE* f = fopen("/proc/self/status", "r");
  if (f == NULL) exit(3);
  char line[256];
  while (fgets(line, sizeof(line), f) != NULL) {
    unsigned long long kb;
    if (sscanf(line, "VmRSS: %llu kB", &kb) == 1) m.rss = kb * 1024;
    if (sscanf(line, "VmSize: %llu kB", &kb) == 1) m.virtual_bytes = kb * 1024;
  }
  fclose(f);
  f = fopen("/proc/self/smaps_rollup", "r");
  if (f != NULL) {
    while (fgets(line, sizeof(line), f) != NULL) {
      unsigned long long kb;
      if (sscanf(line, "Private_Clean: %llu kB", &kb) == 1 ||
          sscanf(line, "Private_Dirty: %llu kB", &kb) == 1) m.private_bytes += kb * 1024;
    }
    fclose(f);
  }
#endif
  return m;
}

static void sample(const char* phase, int wave, double elapsed_ms,
                   int requested, int status, const mi_purge_all_report_t* report) {
  process_memory_t pm = process_memory();
  mi_stats_t_decl(stats);
  if (!mi_stats_get(&stats)) exit(4);
  mi_holes_report_t holes;
  _mi_purge_holes_report_collect(&holes);
  printf("{\"phase\":\"%s\",\"wave\":%d,\"rss_bytes\":%llu,"
         "\"private_bytes\":%llu,\"process_virtual_bytes\":%llu,"
         "\"allocator_reserved_bytes\":%llu,\"allocator_committed_bytes\":%lld,"
         "\"arena_metadata_bytes\":%llu,\"elapsed_ms\":%.6f,"
         "\"reclaim_requested\":%s,\"purge_status\":%d,\"reclaim_pass_ran\":%s,"
         "\"arenas_reclaimed\":%llu,\"arena_reclaim_bytes\":%llu,"
         "\"arenas_kept\":%llu,\"subprocs_pending\":%llu}\n",
         phase, wave, (unsigned long long)pm.rss,
         (unsigned long long)pm.private_bytes, (unsigned long long)pm.virtual_bytes,
         (unsigned long long)holes.arena_reserved_bytes, (long long)stats.committed.current,
         (unsigned long long)holes.arena_meta_bytes, elapsed_ms,
         requested ? "true" : "false", status, report && report->reclaimed ? "true" : "false",
         (unsigned long long)(report ? report->arenas_reclaimed : 0),
         (unsigned long long)(report ? report->arena_reclaim_bytes : 0),
         (unsigned long long)(report ? report->arenas_kept : 0),
         (unsigned long long)(report ? report->subprocs_pending : 0));
  fflush(stdout);
}

static void* alloc_touched(void) {
  unsigned char* p = (unsigned char*)mi_malloc(BIG_BYTES);
  if (p == NULL) exit(5);
  for (size_t i = 0; i < BIG_BYTES; i += 4096) p[i] = (unsigned char)(i / 4096);
  return p;
}

int main(int argc, char** argv) {
  if (argc != 3 || (strcmp(argv[1], "purge-only") != 0 && strcmp(argv[1], "reclaim") != 0) ||
      (strcmp(argv[2], "empty") != 0 && strcmp(argv[2], "nonempty") != 0)) {
    fprintf(stderr, "usage: bench_arena_reclaim {purge-only|reclaim} {empty|nonempty}\n");
    return 2;
  }
  const int reclaim = strcmp(argv[1], "reclaim") == 0;
  const int keep_live = strcmp(argv[2], "nonempty") == 0;
  mi_option_set(mi_option_arena_reserve, (long)(32 * 1024)); /* KiB; one big object / arena */
  mi_scavenger_stop();
  void* small = mi_malloc(64); /* keep ordinary process activity separate from big arenas */
  if (small == NULL) return 5;
  void* blocks[BIG_COUNT];
  sample("baseline", 0, 0.0, 0, 0, NULL);
  for (int wave = 1; wave <= 2; wave++) {
    double start = seconds_now();
    for (int i = 0; i < BIG_COUNT; i++) blocks[i] = alloc_touched();
    sample("peak", wave, (seconds_now() - start) * 1000.0, 0, 0, NULL);
    if (!keep_live) {
      for (int i = 0; i < BIG_COUNT; i++) mi_free(blocks[i]);
    }
    sample("free", wave, 0.0, 0, 0, NULL);
    mi_purge_all_report_t ordinary;
    start = seconds_now();
    int status = mi_purge_all_ex(MI_PURGE_FORCE, 100, &ordinary);
    sample("purge", wave, (seconds_now() - start) * 1000.0, 0, status, &ordinary);
    if (reclaim) {
      mi_purge_all_report_t report;
      start = seconds_now();
      status = mi_purge_all_ex((mi_purge_flags_t)(MI_PURGE_FORCE | MI_PURGE_RECLAIM), 100, &report);
      sample("reclaim", wave, (seconds_now() - start) * 1000.0, 1, status, &report);
    } else {
      sample("reclaim", wave, 0.0, 0, 0, NULL); /* aligned control checkpoint */
    }
    if (keep_live) {
      for (int i = 0; i < BIG_COUNT; i++) mi_free(blocks[i]);
      mi_purge_all_ex(MI_PURGE_FORCE, 100, NULL);
    }
    /* The next wave's peak time is the regrowth cost; two waves expose churn. */
  }
  mi_free(small);
  return 0;
}
