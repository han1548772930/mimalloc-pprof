# Reclaiming the arenas that are completely free — `MI_PURGE_RECLAIM`

This is phase **F** of `mi_purge_all_ex` (#366). The purge itself is documented in
`docs/purge-all.md`, its plan in `docs/purge-all-implementation.md`; this document is about
the part that gives an *arena* back — metadata included — and why it has to prove something
about every other thread before it may.

The problem it solves is a shape rather than a bug: arena metadata is a function of the
allocation **peak**, not of the live set, and nothing before this flag gave it back.

## 1. The problem

A workload that allocates in waves keeps every arena its peak created, and with them the
metadata of those arenas. That metadata is not a function of how much is live — it is a
function of the **peak**, and it does not come back down:

- `MIMALLOC_ARENA_MAX_OBJECT_SIZE` is *prevention*. It keeps a single large object from
  forcing a wider reservation, and it cannot give an already-created arena back: a restart
  was the only thing that did.
- It cannot be derived from process metrics either. `committed - malloc blocks` is not "arena
  metadata"; on Windows `MEM_RESET` keeps pages committed, so a committed figure that does
  not fall after a purge does not mean the objects are still live.
- `purge_all` (issue #366) returns the *contents* of the arenas — pages, holes, abandoned
  pages — and explicitly does not destroy arena structures. That was the missing half.

## 2. Where the memory goes, and what never gives it back

`mi_arena_reserve` (`src/arena.c`) reserves an arena when no existing one can serve a
request, and every arena carries its own bookkeeping in `arena->info_slices`
(`mi_arena_info_slices_needed`): the `mi_arena_t`, the four slice bitmaps, the `slices_free`
b-bitmap, and the dominant part — `pages_meta`, one `mi_page_t` per slice, i.e. **2.8 MiB per
GiB of arena**. `mi_arena_initialize` allocates that block and commits it
(`mi_size_of_slices(info_slices)`), so it is resident for as long as the arena exists, no
matter how little of the arena is in use.

Nothing in a running process releases it:

| what happens | where | effect on the arena |
|---|---|---|
| a block leaves the arena | `_mi_arenas_free` | returns the slices to `arena->slices_free`, schedules a delayed purge |
| that purge runs | `_mi_arenas_try_purge` | *decommits* the freed slices; the arena and its metadata stay |
| a sub-process is destroyed / `destroy_on_exit` | `mi_arenas_unsafe_destroy` | destroys the arenas — not reachable from a live process |
| `mi_arena_unload` | end of `src/arena.c` | a compiled-out sketch (`/* ... */`), exclusive arenas only |

And the arena set only grows: `mi_arena_reserve` scales the next reservation by
`1 << clamp(arena_count/8, 0, 16)`, and while `mi_arenas_add` recycles a NULL slot, nothing
lowers `arena_count` for the lifetime of the process. A long-running service therefore keeps
the address space *and* the metadata of its worst moment, at the widest per-arena size it
ever needed.

## 3. What `MI_PURGE_RECLAIM` does

`mi_purge_all_ex(flags | MI_PURGE_RECLAIM, wait_ms, report)` runs the ordinary walk
(phases A–E, unchanged) and then, as phase F, releases every arena of every sub-process that
is **completely free**:

1. **Candidate.** A data slice in `slices_free` means a page still exists — a live page, an
   abandoned page, a page the heap-delete claim protocol is holding — so
   `mi_bbitmap_is_xsetN(MI_BIT_SET, arena->slices_free, info_slices, slice_count -
   info_slices)` ("every data slice is free") rules out all of them, and with them every
   remaining entry in a heap's `arena_pages[idx]` and every entry of the lazy abandoned
   bitmaps that belongs to the arena. The arena must also be the library's to give back: OS
   memory (not `MI_MEM_EXTERNAL`/static), not a sub-arena of caller-managed memory, not
   pinned, and not `exclusive` (a caller may still hold its `mi_arena_id_t`).
2. **Hand back the per-heap tracking that points into it.** For the main heap that is
   `&arena->pages_main`, whose on-demand `pages_abandoned[bin]` bitmaps are separate raw-OS
   chunks (#350) that the OS free below would otherwise leak; the slot in
   `heap_main->arena_pages[idx]` is cleared first (nothing else ever would: `mi_heap_free`
   skips that loop for a main heap).
3. **Forget it in the page map.** `_mi_page_map_unregister_range(arena,
   mi_size_of_slices(arena->info_slices))` — with every data slice free, the accessed prefix
   is exactly the info block.
4. **Unlist it.** The slot in `subproc->arenas[]` is set to NULL *before* the memory goes, so
   it is immediately reusable by `mi_arenas_add` (which prefers a NULL slot); `arena_count`
   is decremented only when the released arena was the last slot (the same CAS
   `mi_arenas_unsafe_destroy` does).
5. **Return it to the OS.** `_mi_os_free_ex(subproc, arena, mi_size_of_slices(slice_count),
   true, memid)` — the whole reservation, metadata included.
6. **Sweep the leftovers.** A second pass frees any `heap->arena_pages[i]` from another heap
   whose `subproc->arenas[i]` is NULL by now (detached under that heap's own
   `arena_pages_lock`). Leaving one in place would be latent corruption: slot `i` can be
   recycled for a *larger* arena, and the next `mi_bitmap_setN` into a bitmap sized for the
   old `slice_count` writes past its end.

The report says what happened:

| field | meaning |
|---|---|
| `arenas_reclaimed` / `arena_reclaim_bytes` | arenas released to the OS, and the reservation bytes they held |
| `arenas_kept` | seen completely free and **not** released — not the library's to give back (the `exclusive`/pinned/external cases above) |
| `subprocs_pending` | sub-processes where the proof below failed; nothing was released there |
| `reclaimed` | the flag was passed and nothing blocked a pass (no pending sub-process, no other arena pass in flight). Says the pass *ran* — `arenas_reclaimed` says whether it found anything |

## 4. Why it needs quiescence, and how it gets it

An arena that is completely free has no pages, so no page path can touch it. Its **own**
metadata is different: `mi_arenas_try_find_free` walks `subproc->arenas[]` under no lock, and
`mi_arena_try_alloc_at` claims slices with an atomic b-bitmap clear. Freeing an arena a
concurrent allocation can still find is not a purge-sized problem — it is a use-after-free of
the arena's own metadata.

So phase F runs only when it can **prove** that no other thread of the sub-process can be
inside the allocator for the whole duration, using the #366 park protocol in its strict form:

- It claims **every** registered tld of the sub-process at once — the same
  `MI_PARK_PARKED -> MI_PARK_SWEEPING` CAS `mi_purge_walk_claim` uses, held simultaneously
  instead of one at a time. A thread that is inside an allocator call has park_state
  `RUNNING` and cannot be claimed: the pass fails, releases the claims it took, and reports
  the sub-process pending. Nothing is freed, and nothing waits on a state that cannot change.
- A claimed owner blocks in `_mi_park_leave`/`_mi_park_leave_gate` until its claim is
  released, so the claims also keep the claimed tlds (and their theaps) alive for the walk.
- Our own tld is deliberately not claimed: it is the thread running this code, and the driver
  enters the gate around the pass (so a gated build cannot be claimed by the scavenger while
  it frees).
- With `MI_OWNER_GATE=1` a PARKED tld is *proof*: the gate parks a thread whenever it is
  outside the allocator, so "every tld is claimable" is exactly "no thread is inside the
  allocator now", and a `RUNNING` owner is transient. In the default build a PARKED tld is
  only a *promise* — a thread parks when it calls `mi_on_thread_idle_start` — so a reclaim
  there is limited to cooperative-idle processes; an ungated owner that allocates while
  parked already races the #272 sweep in exactly the same way.
- `wait_ms` bounds the phase's own retry for an owner that is merely between two allocator
  calls (spin, then yield). A pass that got its claims runs to completion; a sub-process that
  never becomes claimable stays in the report.

Two more things could enter the allocator while those claims are held, and each is closed by
a lock held for the whole pass (`sp->heaps_lock`, `sp->tlds_lock`, `sp->theap_meta_lock`):

- **A thread that does not exist yet.** It bootstraps through `_mi_meta_zalloc` *before* it is
  registered in `sp->tlds`, and its meta comes from the detached `sp->theap_meta` theap. Every
  meta allocation holds `sp->theap_meta_lock` across the whole allocation, so a bootstrapping
  thread blocks at its first one. A thread already inside `_mi_thread_init_with_heap` is
  visible with park_state `RUNNING` instead, which fails the claim pass — the conservative
  answer.
- **The background scavenger's timer** (`mi_scavenger_run` -> `_mi_arenas_try_purge`), which
  reads and writes arena bitmaps with no park claim involved. Phase F holds the arena purge
  guard for the whole pass, and the scavenger's `mi_atomic_guard` is non-blocking, so it
  skips; its sweep is impossible anyway, since a sweep needs a PARKED tld and we hold them
  all.

## 5. Lock order

Inside the pass: `subprocs_lock` (the caller's) -> `heaps_lock` -> `tlds_lock` ->
`theap_meta_lock`, plus the arena purge guard, which is a leaf (`mi_arenas_purge_guard`) —
the documented order of `src/fork.c` (2 -> 4 -> 7) with the meta theap last, as
`_mi_meta_zalloc` takes it. Nothing in the pass waits for a lock it holds, the per-heap
leftover sweep releases `theap_meta_lock` before it frees (because `_mi_free_subproc_safe`
may take it), and the OS free is the last thing done with an arena.

## 6. Accepted limits

- It is a **flag on `mi_purge_all_ex`**, not a new entry point: that call already owns the
  purge admission and the walk, so two reclaims cannot overlap.
- **Exclusive arenas are never released.** An arena reserved with `exclusive = true` (or a
  `mi_arena_id_t` the caller asked for) is one the caller may still name; releasing it would
  be a use-after-free of the *id*. Keep `exclusive = true` for arenas whose ids outlive the
  call, or drop the id before reclaiming.
- A sub-process with a RUNNING owner (or an orphan tld) is **reported, not waited for**, and
  the report says so through `subprocs_pending`.
- When the arena layer itself is busy, an attempt is skipped and retried; if the budget runs
  out that way, the report says so through `layer_busy` (internal) and `reclaimed == false`
  rather than inventing a pending sub-process.
- `subproc->arena_count`'s CAS shrinks the live count only for the last slot, and the
  subproc's arena-count *statistic* (`mi_subproc_stat_counter`) is not touched: it counts
  arenas ever added, not arenas alive.
- A parked thread must not call `mi_on_thread_idle` — that is an allocator call, and doing it
  while parked races the sweep.

## 7. Using it

The idiom is "reclaim at a quiescent point", which is exactly when a service's worker threads
hand their theaps over (`mi_on_thread_idle_start`):

```c
#include <mimalloc.h>

/* Give back the arenas the peak left behind. Best effort by construction: a caller that
   cares re-checks the report (and may run this again later). Returns the bytes returned
   to the OS, or 0 when another purge was in flight. */
static size_t reclaim_free_arenas(void) {
  mi_purge_all_report_t r;
  const int status = mi_purge_all_ex((mi_purge_flags_t)(MI_PURGE_FORCE | MI_PURGE_RECLAIM),
                                     100, &r);
  if (status == MI_PURGE_BUSY) { return 0; }   /* nothing was done */
  return r.arena_reclaim_bytes;
}
```

Rust: `mimalloc_pprof::purge_all_ex(mimalloc_pprof::PurgeFlags::FORCE_RECLAIM, 100)`, and the
same fields on `PurgeAllReport` (`arenas_reclaimed`, `arena_reclaim_bytes`, `arenas_kept`,
`subprocs_pending`, `reclaimed`). From the crate, `purge_all(force)` is the C convenience form
and never reclaims.

Two things make a reclaim more likely to succeed, and both are the same advice the purge
already has: call it when the other threads are between two allocator calls (idle hand-off,
queue-empty), and do not hold other locks while calling it. On a build with `owner-gate`,
every thread outside the allocator is claimable and the pass succeeds as soon as no call is
in flight.

## 8. Tests

`test/test-arena-reclaim.cpp` (ctest `test-arena-reclaim`, `RUN_SERIAL`: the rows assert on
**process-wide** arena accounting and A1 requires it to start at zero). The rows:

| row | claim |
|---|---|
| A1 | the workload really reserves extra arenas: one 32 MiB object per 96 MiB arena, measured per iteration (`mi_option_arena_reserve` is in KiB), and the total is exactly the sum of them |
| A2 | the control: **without** the flag nothing is released — accounting byte-for-byte unchanged (the behaviour before it) |
| A3 | with the flag, every completely free arena goes, `arenas_kept == 0`, the accounted bytes drop by exactly `arena_reclaim_bytes`, and the live objects in the arenas that are *not* free survive byte-for-byte |
| A4 | after the last live object is freed the remaining arenas of the peak go too: the metadata follows the live set, not the process peak |
| A5 | (debug builds, `MI_DEBUG > 0`) a thread **inside** the allocator — stalled in `mi_heap_delete` between the pin and the claim — makes the pass refuse: `reclaimed == false`, nothing released, accounting untouched; once it is out, the same arena is released |
| A6 | a live but **parked** thread (`mi_on_thread_idle_start`) does not block it: the claim protocol reaches it, and the parked thread's own allocation is never released under it |

All three builds the fork gates on are exercised locally:

```text
cl /DMI_DEBUG=3                         # ungated debug      -> PASSED
cl /DMI_DEBUG=0                         # ungated release     -> PASSED
cl /DMI_DEBUG=3 /DMI_OWNER_GATE=1       # gated               -> PASSED
```

The numbers a `-DMI_DEBUG=3` run prints on Windows x64 (`A1` reserves 448 MiB, metadata
1.438 MiB; A3/A4 reclaim 2+2 arenas / 192 MiB each, metadata falling to 0.188 MiB) show the
shape of the problem in miniature: metadata that tracks the live set instead of the peak is
what this flag is for.
