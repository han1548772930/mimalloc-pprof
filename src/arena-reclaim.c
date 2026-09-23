/* Reclaim the arenas that are completely free: phase F of `mi_purge_all_ex`, driven by
   `MI_PURGE_RECLAIM`.

   THE PROBLEM

   An arena is reserved on demand (`mi_arena_reserve`, src/arena.c) and NOTHING gives it back:
   a block that leaves an arena only returns its slices to `arena->slices_free` and schedules a
   (delayed) purge (`_mi_arenas_free`), and the purge only *decommits* the slices that were
   freed -- it never touches the arena's own bookkeeping. The only paths that destroy an arena
   are `mi_arenas_unsafe_destroy` (src/arena.c, reached from `mi_option_destroy_on_exit` and
   `mi_subproc_destroy`) and process exit.

   What that leaves behind per arena is its whole metadata, `arena->info_slices`
   (`mi_arena_info_slices_needed`): the `mi_arena_t` itself, the four slice bitmaps, the
   `slices_free` bbitmap and -- the dominant part -- `pages_meta`, one `mi_page_t` per slice
   (`slice_count * sizeof(mi_page_t)`, 2.8 MiB per GiB of arena). The metadata is reserved, and
   on an OS without overcommit it is COMMITTED at initialization (`mi_arena_initialize` commits
   `mi_size_of_slices(info_slices)`), i.e. it costs resident memory for as long as the arena
   exists, no matter how little of the arena is in use.

   In a wave-shaped workload (allocate a lot, free it all, allocate again) that metadata is a
   function of the PEAK, not of the current live set: a spike that has been fully freed still
   holds every arena it created, and their 2.8 MiB/GiB, until the process exits. Releasing the
   arenas the peak left behind is the point of this pass.

   Arena size makes it worse: `mi_arena_reserve` scales its reservation by
   `1 << clamp(arena_count/8, 0, 16)`, so each new arena is bigger (and its metadata with it),
   and `arena_count` has no other way down: `mi_arenas_add` recycles a NULL slot but nothing
   ever calls `mi_arenas_unsafe_destroy` for a live process.

   WHAT THIS FILE DOES

   `MI_PURGE_RECLAIM` (issue #366 phase F, driven from `mi_purge_all_ex` in src/purge-all.c)
   releases every arena of every sub-process that is COMPLETELY FREE back to the OS: the
   per-heap tracking that points into it is handed back first, the arena leaves the page map
   and the subproc's arena table, and `_mi_os_free_ex` returns the reservation -- metadata
   included -- to the OS. The freed slot is immediately reusable by `mi_arenas_add` (it already
   prefers a NULL entry), so the next peak starts at `arena_count` (and hence the exponential
   reservation scale) where the process really is, not where its worst moment was.

   WHY IT NEEDS QUIESCENCE, AND HOW IT GETS IT

   An arena that is completely free has no pages, so no *page* path can touch it -- but the
   arena's own bitmaps and its slot in `subproc->arenas[]` are lock-free state that an ordinary
   allocation can reach at any moment (`mi_arenas_try_find_free` walks the slots under no lock,
   `mi_arena_try_alloc_at` claims slices with an atomic bbitmap clear). Freeing an arena that a
   concurrent allocation can still find is therefore not a purge-sized problem: it is a
   use-after-free of the arena's metadata.

   So the reclaim refuses to run unless it can PROVE that no other thread of the sub-process
   can be inside the allocator, for the whole duration. It uses the #366 park protocol for
   that, in its strict form: it claims EVERY registered tld of the sub-process at once (the
   same `MI_PARK_PARKED -> MI_PARK_SWEEPING` CAS `mi_purge_walk_claim` uses, only all of them
   instead of one). A claimed owner is blocked in `_mi_park_leave_gate`/`_mi_park_leave` until
   we release it, so an owner that is inside an allocator call has park_state RUNNING and
   cannot be claimed -- the pass fails and NOTHING is freed. That is the whole contract: a
   RUNNING owner (or an orphan, or a thread whose own tld we cannot classify) makes the call a
   no-op for that sub-process, never a hang and never a race.

   What PARKED buys differs between the two builds, and that difference is the #366 one:

     - with MI_OWNER_GATE the gate parks a thread whenever it is outside the allocator, so a
       PARKED tld is PROOF that its owner holds no allocator state, and "every tld of the
       sub-process is claimable" is exactly "no thread is inside the allocator right now".
       A RUNNING owner is then a transient -- it is inside a call -- and the bounded retry
       below rides it out.
     - in the default build PARKED is a PROMISE (`mi_on_thread_idle_start`: will not allocate
       or free until `mi_on_thread_idle_end`, src/scavenger.c), not a proof: an ungated owner
       that allocates while parked already races the #272 sweep in exactly the same way, so
       phase F adds no new precondition here -- but it cannot work without one either. Only a
       cooperative-idle process can be reclaimed in this build; a thread that never parks (or
       never parks long enough to be caught by the retry) leaves its sub-process in the
       pending report. A parked thread must not enter `mi_on_thread_idle` either: that is an
       allocator call (it sweeps its own theaps), and doing it while parked races the sweep.

   Two more things could enter the allocator while we hold those claims, and each is closed by
   a lock taken for the whole pass:

     - a thread that does not exist yet (or is in its first allocator call) bootstraps itself
       through `_mi_meta_zalloc` BEFORE it is registered in `sp->tlds` (init.c: `mi_tld_create`
       allocates the tld, `mi_tld_init` registers it), and its theap/tld come from the detached
       `sp->theap_meta` theap, whose heap is `heap_main`. So `sp->tlds_lock` alone is not
       enough; holding `sp->theap_meta_lock` is: every meta allocation holds it across the
       whole allocation (subproc.c), so a bootstrapping thread blocks at its first one.
       A thread that is ALREADY inside `_mi_thread_init_with_heap` when we start is visible in
       `sp->tlds` with park_state RUNNING (mi_tld_init publishes RUNNING, `_mi_thread_init_with_heap`
       stores PARKED only as its last act) -- i.e. our claim pass fails on it, which is the
       conservative answer we want.
     - the background scavenger's timer, which calls `_mi_arenas_try_purge` on
       `subproc->purge_expire` with no park claim involved (src/scavenger.c `mi_scavenger_run`)
       and would read/write the bitmaps of an arena we are freeing. Holding the arena purge
       guard for the whole pass makes it skip (`mi_atomic_guard` is non-blocking): its sweep is
       already impossible, since sweeping requires claiming a PARKED tld and we hold them all.

   Lock order is the documented one (src/fork.c: subprocs -> heaps -> tlds -> ... -> theap_meta,
   page_map/arena-reserve under theap_meta). Nothing in the pass waits for a lock it holds, and
   the OS frees are the last thing done with each arena.

   ACCEPTED LIMITS (each one a consequence of the design above, not a TODO)

     - Not a public entry point: it is a flag on `mi_purge_all_ex`, which already owns the
       purge admission (`_mi_purge_admission`) and the walk. Two reclaims cannot overlap.
     - A sub-process with a RUNNING owner (or an orphan tld) is reported, not reclaimed. The
       pass does retry within its own `wait_ms` budget (below) to ride out an owner that is
       merely between two allocator calls, but it never waits on a lock it holds and never
       spins on a state that cannot change -- a caller stays in control and simply calls again
       at its next quiescent point.
     - `wait_ms` bounds that retry only: the acquisition wait for park states. A pass that got
       its claims runs to completion (there is nothing to pace here, unlike the hole sweep, and
       the pass holds the arena purge guard while it frees).
     - When the ARENA LAYER itself is busy (`_mi_arenas_purge_guard` held by the scavenger or by
       another thread's purge), an attempt is skipped and retried. If the budget runs out that
       way, the report says so through `mi_arena_reclaim_report_t::layer_busy` instead of
       inventing a pending sub-process.
     - Only arenas with no data slice in use and no used per-heap tracking are candidates, so a
       heap that holds `arena_pages[idx]` for one -- the ordinary state after a peak -- is
       cleaned up in the same call (see `mi_arena_reclaim_release_heap_pages`).
     - The caller's own tld is deliberately NOT claimed: it is the one running this code, and
       its theaps are not swept by anyone while it holds its own gate (the driver enters it).
     - An arena the application reserved itself (`mi_reserve_os_memory_ex`) and still holds a
       `mi_arena_id_t` for is NOT distinguishable from one the allocator reserved, unless it was
       reserved `exclusive` (those are skipped). Retaining an arena id across a reclaim and then
       using it is a use-after-free; the supported pattern is `exclusive = true` for arenas whose
       ids outlive the call.
     - `subproc->arena_count` shrinks only when the released arena was the last slot (the same
       CAS the `mi_arena_unload` sketch at the end of src/arena.c -- currently compiled out --
       and `mi_arenas_unsafe_destroy` do), and the subproc's `arena_count` STATISTIC
       (`mi_subproc_stat_counter`) is not touched at all: it counts arenas ever added, not
       arenas alive.

   Nothing here is on an allocation fast path: the pass runs only inside `mi_purge_all_ex` when
   the caller passes `MI_PURGE_RECLAIM`. There is no build option for it -- a build whose
   callers never pass the flag pays one flag test per `mi_purge_all_ex` call and nothing else.
*/

#include "mimalloc.h"
#include "mimalloc/internal.h"
#include "mimalloc/prim.h"   // _mi_prim_thread_yield (the phase D ramp, as in src/purge-all.c)
#include "mimalloc/types.h"
#include "bitmap.h"          // mi_bbitmap_is_xsetN

size_t mi_arenas_get_count(mi_subproc_t* subproc);   // src/arena.c (not in internal.h, like src/heap-snapshot.c)

// #272/#366 (MSVC-C): the MSVC C atomics wrapper is word-width only, so every CAS out-param
// here is a `size_t`/`uintptr_t` local, never a narrower one (see src/purge-all.c).
typedef size_t mi_reclaim_park_state_t;


/* -----------------------------------------------------------
  Is one arena a candidate?  (read-only, no locks needed yet)
----------------------------------------------------------- */

// `mi_arena_start(arena)` is `(uint8_t*)arena` (src/arena.c): the `mi_arena_t` -- and the whole
// info block -- lives at the front of the arena's own reservation. `mi_arena_size` is the
// registered reservation size (`mi_size_of_slices(arena->slice_count)`).
static size_t mi_arena_reclaim_size(mi_arena_t* arena) {
  return mi_size_of_slices(arena->slice_count);
}

// The arena is completely free: every DATA slice is in `slices_free`. The info slices are
// never in that bitmap (they are reserved once at initialization:
// `mi_bbitmap_unsafe_setN(arena->slices_free, info_slices, slice_count - info_slices)` in
// `mi_arena_initialize`, src/arena.c), so the test starts at `info_slices` -- which is also
// `mi_arena_used_slices`' definition of an arena with nothing in use.
//
// A page owns its slices for as long as it exists -- including a full page, an abandoned page
// and a page the heap-delete claim protocol is holding -- so this single test rules out every
// live page of this arena, every remaining entry in a heap's `arena_pages[idx]`, and every
// entry of the lazy abandoned maps that belongs to it.
static bool mi_arena_reclaim_is_empty(mi_arena_t* arena) {
  const size_t slice_count = arena->slice_count;
  const size_t info_slices = arena->info_slices;
  if (slice_count <= info_slices) return false;
  return mi_bbitmap_is_xsetN(MI_BIT_SET, arena->slices_free, info_slices, slice_count - info_slices);
}

// May we release this arena's memory at all?
//   - `mi_memkind_is_os`: the arena came from `mi_reserve_os_memory_ex2` (either
//     `mi_arena_reserve`'s own path or the public `mi_reserve_os_memory_ex`). MI_MEM_EXTERNAL
//     (`mi_manage_os_memory`) and MI_MEM_STATIC memory belongs to the caller.
//   - `parent == NULL`: a sub-arena of a managed area (`mi_manage_os_memory_ex2`) is a slice of
//     the caller's memory too.
//   - `!is_pinned`: pinned (huge/large-page) memory cannot be handed back per page.
//   - `!is_exclusive`: an exclusive arena is one whose `mi_arena_id_t` the caller asked for and
//     may still hold (`mi_reserve_os_memory_ex(..., exclusive=true)`, `mi_heap_new_ex`).
// This is exactly the set `mi_arenas_unsafe_destroy` frees, plus the `is_exclusive` guard that
// upstream's `mi_arena_unload` uses for the same reason.
static bool mi_arena_reclaim_is_ours(mi_arena_t* arena) {
  return (mi_memkind_is_os(arena->memid.memkind) &&
          arena->parent == NULL &&
          !arena->memid.is_pinned &&
          !arena->is_exclusive);
}

/* -----------------------------------------------------------
  The per-arena body
----------------------------------------------------------- */

// Called with every other thread of `subproc` blocked out of the allocator, `sp->heaps_lock`,
// `sp->tlds_lock`, `sp->theap_meta_lock` and the arena purge guard held. Returns true if the
// arena was released.
static bool mi_arena_reclaim_one(mi_subproc_t* subproc, size_t idx, mi_arena_t* arena,
                                 mi_arena_reclaim_report_t* rep)
{
  // Order matters for the report: `arenas_kept` counts what was COMPLETELY FREE and still left
  // in place -- a live arena (the ordinary state after a peak) is not a complaint, and an arena
  // nobody asked to give back (see `mi_arena_reclaim_is_ours`) is not one either as long as it
  // holds data. The emptiness test is the cheap one to fail: it is a bitmap query over the
  // slices the arena actually has.
  if (!mi_arena_reclaim_is_empty(arena)) return false;
  if (!mi_arena_reclaim_is_ours(arena)) { rep->arenas_kept++; return false; }

  // The main heap's per-arena tracking IS the arena: `mi_heap_ensure_arena_pages` stores
  // `&arena->pages_main` for it, and its on-demand `pages_abandoned[bin]` bitmaps are separate
  // raw-OS chunks (#350) that the OS free below would leak. Both go before the memory does --
  // and `heap_main->arena_pages[idx]` must be cleared, or the main heap would keep a pointer
  // into the freed reservation (`mi_heap_free` skips the arena-pages loop for a main heap,
  // heap.c, so nothing else would ever clear it).
  mi_heap_t* const heap_main = mi_atomic_load_ptr_acquire(mi_heap_t, &subproc->heap_main);
  if (heap_main != NULL) {
    mi_arena_pages_t* const main_pages =
      mi_atomic_load_ptr_acquire(mi_arena_pages_t, &heap_main->arena_pages[idx]);
    if (main_pages != NULL) {
      if (main_pages != &arena->pages_main) { rep->arenas_kept++; return false; }   // impossible; never free a foreign structure
      _mi_arena_pages_free_abandoned(&arena->pages_main);
      mi_atomic_store_ptr_release(mi_arena_pages_t, &heap_main->arena_pages[idx], NULL);
    }
  }

  // The page map must forget the arena before the memory goes. `asize` is the *accessed*
  // prefix, exactly what upstream's `mi_arena_unload` unregisters: with every data slice free
  // that is the info block, and `mi_arena_reclaim_is_empty` says no page of this arena was
  // ever registered beyond it (`mi_arena_page_register`, src/arena.c).
  _mi_page_map_unregister_range((void*)arena, mi_size_of_slices(arena->info_slices));

  // Unlist before releasing: the slot is then immediately reusable by `mi_arenas_add`, which
  // prefers a NULL entry, and `arena_count` only shrinks when this was the last one (the same
  // CAS `mi_arena_unload` and `mi_arenas_unsafe_destroy` do).
  const mi_memid_t memid = arena->memid;
  const size_t asize = mi_arena_reclaim_size(arena);
  mi_atomic_store_ptr_release(mi_arena_t, &subproc->arenas[idx], NULL);
  const size_t count = mi_arenas_get_count(subproc);
  if (idx + 1 == count) {
    size_t expected = count;
    mi_atomic_cas_strong_acq_rel(&subproc->arena_count, &expected, count - 1);
  }

  // And back to the OS -- reservation, metadata and all. `still_committed = true` mirrors
  // `mi_arenas_unsafe_destroy`: the arena's own memory was committed when it was initialized
  // (`mi_arena_initialize` commits the info block, and an initially-committed arena counts the
  // whole reservation), so the subproc's committed statistics must drop by the same amount.
  _mi_os_free_ex(subproc, (void*)arena, asize, true /* still committed */, memid);

  rep->arenas_reclaimed++;
  rep->reclaim_bytes += asize;
  return true;
}

/* -----------------------------------------------------------
  Per-heap tracking of a released arena
----------------------------------------------------------- */

// Every non-main heap of the sub-process may still hold an `arena_pages[idx]` for an arena that
// is now gone: `mi_heap_ensure_arena_pages` allocated it the first time the heap put a page in
// that arena, and only `mi_heap_free` would ever free it (heap.c -> `_mi_arena_pages_free`).
// Leaving it in place would be a latent corruption: slot `idx` can be recycled by
// `mi_arenas_add` for a *different*, larger arena, and the next
// `mi_bitmap_setN(arena_pages->pages, slice_index, ...)` in that arena writes past the end of a
// bitmap that was sized for the old `slice_count`.
//
// A slot whose arena is NULL can only be such a leftover: every writer of
// `heap->arena_pages[i]` first had the arena at slot `i` (`mi_heap_ensure_arena_pages` is
// called with an arena found in the table), and both are cleared together here. The pass is
// safe without `theap_meta_lock` -- `mi_arena_pages_free_abandoned` frees raw-OS chunks and
// `_mi_free_subproc_safe` may collect, i.e. it may take `theap_meta_lock` itself -- because the
// claims keep every other thread out of `mi_heap_ensure_arena_pages` and out of `mi_heap_free`.
static void mi_arena_reclaim_release_heap_pages(mi_subproc_t* subproc, mi_heap_t* heap_main)
{
  const size_t count = mi_arenas_get_count(subproc);
  for (mi_heap_t* heap = subproc->heaps; heap != NULL; heap = heap->next) {
    if (heap == heap_main) continue;   // handled per arena (`&arena->pages_main`), and it never allocates one
    for (size_t i = 0; i < count; i++) {
      if (mi_atomic_load_ptr_acquire(mi_arena_t, &subproc->arenas[i]) != NULL) continue;
      mi_arena_pages_t* arena_pages = mi_atomic_load_ptr_acquire(mi_arena_pages_t, &heap->arena_pages[i]);
      if (arena_pages == NULL) continue;
      // Detach under the heap's own lock, then free. `mi_heap_ensure_arena_pages` publishes
      // under this lock, and no one else can take the pointer from here on.
      mi_lock(&heap->arena_pages_lock) {
        if (mi_atomic_load_ptr_acquire(mi_arena_pages_t, &heap->arena_pages[i]) == arena_pages) {
          mi_atomic_store_ptr_release(mi_arena_pages_t, &heap->arena_pages[i], NULL);
        }
        else {
          arena_pages = NULL;   // another thread published one; leave it (impossible while we hold the claims)
        }
      }
      if (arena_pages != NULL) { _mi_arena_pages_free(arena_pages); }
    }
  }
}

/* -----------------------------------------------------------
  The claim pass: the strict form of the #366 walk
----------------------------------------------------------- */

// Claim EVERY tld of `sp` at once -- the same `MI_PARK_PARKED -> MI_PARK_SWEEPING` CAS
// `mi_purge_walk_claim` (src/purge-all.c) performs, held simultaneously instead of one at a
// time. Returns false, having released everything it took, if any single tld cannot be claimed:
// a RUNNING owner (a thread inside the allocator right now, a thread mid-init, a thread in its
// own teardown), or an orphan of a pre-fork thread, which is never parked and whose state we
// cannot classify. The caller holds `sp->tlds_lock` (this holds no lock itself).
static bool mi_arena_reclaim_claim_all(mi_subproc_t* sp, mi_tld_t* my_tld)
{
  const uintptr_t me = (uintptr_t)_mi_thread_id();
  mi_tld_t* const scav_tld = _mi_scavenger_tld_ptr();   // NULL in every build without a scavenger tld (Windows DLL only)
  bool ok = true;
  for (mi_tld_t* tld = sp->tlds; tld != NULL; tld = tld->subproc_next) {
    if (tld == my_tld) continue;                        // we are inside the allocator (the driver entered the gate)
    if (scav_tld != NULL && tld == scav_tld) continue;  // the scavenger owns nothing and never parks
    if ((mi_atomic_load_relaxed(&tld->gate_flags) & (size_t)MI_GATE_FLAG_ORPHAN) != 0) { ok = false; break; }
    mi_reclaim_park_state_t expected = MI_PARK_PARKED;
    if (!mi_atomic_cas_strong_acq_rel(&tld->park_state, &expected, MI_PARK_SWEEPING)) { ok = false; break; }
    mi_atomic_store_release(&tld->sweeper, me);
  }
  if (!ok) {
    // Nothing may stay claimed on a failed pass: hand every claim we DID take back to PARKED.
    // `sweeper` names us, which is what makes the walk below safe (the claim keeps each tld
    // alive: its owner blocks in `_mi_park_leave`/`_mi_park_leave_gate` until it is cleared).
    for (mi_tld_t* tld = sp->tlds; tld != NULL; tld = tld->subproc_next) {
      if (mi_atomic_load_acquire(&tld->sweeper) != me) continue;
      mi_atomic_store_release(&tld->sweeper, (uintptr_t)0);
      mi_atomic_store_release(&tld->park_state, (size_t)MI_PARK_PARKED);
    }
  }
  return ok;
}

// Release every claim we hold. The caller still holds `sp->tlds_lock`, so the list cannot change
// under us (and a claimed tld cannot be unregistered: its owner is blocked on the claim).
static void mi_arena_reclaim_release_all(mi_subproc_t* sp)
{
  const uintptr_t me = (uintptr_t)_mi_thread_id();
  for (mi_tld_t* tld = sp->tlds; tld != NULL; tld = tld->subproc_next) {
    if (mi_atomic_load_acquire(&tld->sweeper) != me) continue;
    // Order as in `_mi_purge_walk_sweep`: the holder first, then the state -- a `_mi_gate_held`
    // that reads PARKED never consults `sweeper`.
    mi_atomic_store_release(&tld->sweeper, (uintptr_t)0);
    mi_atomic_store_release(&tld->park_state, (size_t)MI_PARK_PARKED);
  }
}

/* -----------------------------------------------------------
  The driver
----------------------------------------------------------- */

// Reclaim the completely free arenas of one sub-process. Returns true when the pass RAN (every
// thread of the sub-process could be claimed; whether there was anything to free is in `rep`),
// false when the sub-process is left untouched and pending.
//
// Note the shape of `mi_lock`: it is a for-loop whose increment is the release, so NO path may
// `return` out of one of these bodies -- a leaked `heaps_lock` hangs the next purge instead of
// failing loudly. Every early decision is a value that is returned after the lock scopes end.
static bool mi_arena_reclaim_subproc(mi_subproc_t* sp, mi_tld_t* my_tld, mi_arena_reclaim_report_t* rep)
{
  if (sp == NULL) return true;
  const size_t count = mi_arenas_get_count(sp);
  if (count == 0) return true;

  bool complete = false;
  // Fork order 2 -> 4 -> 7 (src/fork.c): the heap list, the tld registry and the meta theap.
  // `heaps_lock` also keeps `sp->heaps` stable for the second pass below.
  mi_lock(&sp->heaps_lock) {
    mi_lock(&sp->tlds_lock) {
      if (mi_arena_reclaim_claim_all(sp, my_tld)) {
        complete = true;
        mi_lock(&sp->theap_meta_lock) {
          for (size_t i = 0; i < count; i++) {
            mi_arena_t* const arena = mi_atomic_load_ptr_acquire(mi_arena_t, &sp->arenas[i]);
            if (arena != NULL) { (void)mi_arena_reclaim_one(sp, i, arena, rep); }
          }
        }
        // The slots of everything we released are NULL by now; hand the per-heap leftovers back.
        mi_heap_t* const heap_main = mi_atomic_load_ptr_acquire(mi_heap_t, &sp->heap_main);
        mi_arena_reclaim_release_heap_pages(sp, heap_main);
        mi_arena_reclaim_release_all(sp);
      }
    }
  }
  return complete;
}

// The entry point for `mi_purge_all_ex` (src/purge-all.c, phase F): reclaim what is quiescent
// NOW and retry within `wait_ms` what is not yet (see "ACCEPTED LIMITS" above). `rep` is filled
// in even when the flag was off, and always says whether the arena layer was ever busy.
void _mi_arenas_reclaim_now(mi_tld_t* my_tld, size_t wait_ms, mi_arena_reclaim_report_t* rep)
{
  if (rep == NULL) return;
  const mi_msecs_t deadline = _mi_clock_now() + (mi_msecs_t)wait_ms;
  for (size_t attempt = 0; ; attempt++) {
    // Exactly one reclaim at a time: the arena purge guard is what the background scavenger's
    // timer -- and any thread's forced purge -- takes before it reads `subproc->arenas[]` and
    // touches an arena's bitmaps (`mi_atomic_guard` is non-blocking, so an opportunistic purge
    // skips; a forced one waits for the guard, which is why it is only held around a pass).
    if (_mi_arenas_purge_guard_acquire()) {
      size_t pending = 0;
      rep->layer_busy = false;
      mi_lock(_mi_subprocs_lock()) {
        for (mi_subproc_t* sp = _mi_subprocs_head(); sp != NULL; sp = sp->next) {
          if (!mi_arena_reclaim_subproc(sp, my_tld, rep)) { pending++; }
        }
      }
      _mi_arenas_purge_guard_release();
      rep->subprocs_pending = pending;
      if (pending == 0) break;
    }
    else {
      rep->layer_busy = true;   // nothing was released for it; `reclaimed` reports false
    }
    if (_mi_clock_now() >= deadline) break;
    // A RUNNING owner may be a thread between two allocator calls (inside one it stays RUNNING
    // for the whole call, and the retry only helps once it finishes). Spin briefly without a
    // syscall, then yield the CPU to it: the phase D ramp (src/purge-all.c).
    if (attempt < 256) { mi_atomic_pause(); } else { _mi_prim_thread_yield(); }
  }
}
