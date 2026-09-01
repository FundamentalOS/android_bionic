/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Functions that bionic's native allocator dispatch needs but that mimalloc
// does not provide directly (or that need Android-specific behavior). Mirrors
// bionic/jemalloc_wrapper.cpp. All symbols are hidden and prefixed mi_* so they
// slot into malloc_common.h's `#define Malloc(function) mi_ ## function` path.
//
// Verified against mimalloc v2.2.4 (include/mimalloc.h). If you bump the
// vendored version, re-check the signatures touched below:
//   mi_memalign, mi_aligned_alloc, mi_usable_size, mi_process_info,
//   mi_option_set/mi_option_purge_delay, mi_collect, mi_stats_print,
//   mi_heap_get_default, mi_heap_visit_blocks, mi_block_visit_fun.

#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdbit.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>

#include <async_safe/log.h>
#include <private/MallocXmlElem.h>

#include "mimalloc.h"

// ---------------------------------------------------------------------------
// memalign / aligned_alloc wrappers
// mimalloc.h #defines these names to our wrapper symbols; undo the macro here
// so the bodies can call the real mimalloc implementation.
// ---------------------------------------------------------------------------
#ifdef mi_memalign
#undef mi_memalign
#endif

void* mi_memalign_round_up_boundary(size_t boundary, size_t size) {
  if (boundary == 0) {
    boundary = 1;
  }
  // Round a non power-of-two boundary up to the next power of two, matching
  // glibc/dlmalloc behavior, because mi_memalign requires a power of two.
  boundary = stdc_bit_ceil(boundary);
  return mi_memalign(boundary, size);
}

#ifdef mi_aligned_alloc
#undef mi_aligned_alloc
#endif

void* mi_aligned_alloc_wrapper(size_t alignment, size_t size) {
  // C11 requires size to be an integral multiple of alignment. mimalloc does
  // not enforce this, so enforce it here (matches the jemalloc wrapper).
  if (alignment == 0 || (size % alignment) != 0) {
    errno = EINVAL;
    return nullptr;
  }
  return mi_aligned_alloc(alignment, size);
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// mallinfo: mimalloc has no malloc(3)-style breakdown. Report committed bytes
// as a coarse approximation of the in-use total. Callers that need precise
// per-bin data are not served by mimalloc; this is best effort.
// ---------------------------------------------------------------------------
struct mallinfo mi_mallinfo() {
  struct mallinfo info;
  memset(&info, 0, sizeof(info));

  size_t elapsed_msecs, user_msecs, system_msecs;
  size_t current_rss, peak_rss, current_commit, peak_commit, page_faults;
  mi_process_info(&elapsed_msecs, &user_msecs, &system_msecs, &current_rss, &peak_rss,
                  &current_commit, &peak_commit, &page_faults);

  // bionic's struct mallinfo members are size_t.
  info.arena = current_commit;     // non-mmapped space allocated from system
  info.uordblks = current_commit;  // total allocated space (approx)
  info.hblkhd = peak_commit;       // space in mmapped regions (approx)
  return info;
}

// ---------------------------------------------------------------------------
// malloc_iterate: enumerate in-use blocks that fall within [base, base+size).
//
// LIMITATION (documented): mimalloc keeps a private heap per thread. The v2.x
// public API can enumerate the *calling thread's* default heap via
// mi_heap_visit_blocks, but there is no supported way to enumerate every live
// thread's heap. This implementation therefore reports the calling thread's
// heap only. That is adequate when the process has been paused and the walk
// runs on the owning thread, but whole-process heap walks (heapprofd,
// memory_replay, some debuggerd paths) will see a partial view. Application
// correctness is unaffected; only heap-inspection tooling is impacted. This is
// acceptable for a flag-gated allocator A/B evaluation.
// ---------------------------------------------------------------------------
namespace {

struct IterateContext {
  uintptr_t base;
  uintptr_t end;
  void (*callback)(uintptr_t base, size_t size, void* arg);
  void* arg;
};

bool IterateVisitBlock(const mi_heap_t* /*heap*/, const mi_heap_area_t* /*area*/, void* block,
                       size_t block_size, void* arg) {
  // block == nullptr indicates the per-area summary callback; skip it.
  if (block == nullptr) {
    return true;
  }
  auto* ctx = static_cast<IterateContext*>(arg);
  uintptr_t ptr = reinterpret_cast<uintptr_t>(block);
  if (ptr >= ctx->base && ptr < ctx->end) {
    ctx->callback(ptr, block_size, ctx->arg);
  }
  return true;  // continue visiting
}

}  // namespace

int mi_malloc_iterate(uintptr_t base, size_t size,
                      void (*callback)(uintptr_t base, size_t size, void* arg), void* arg) {
  IterateContext ctx{base, base + size, callback, arg};
  mi_heap_visit_blocks(mi_heap_get_default(), /*visit_blocks=*/true, &IterateVisitBlock, &ctx);
  return 0;
}

// ---------------------------------------------------------------------------
// malloc_disable / malloc_enable: bionic uses these to quiesce the allocator
// around a heap walk / fork. mimalloc exposes no global allocator lock and is
// designed to be fork-safe without one, so there is nothing to freeze. These
// are intentional no-ops; see the malloc_iterate limitation above.
// ---------------------------------------------------------------------------
void mi_malloc_disable() {}
void mi_malloc_enable() {}

// ---------------------------------------------------------------------------
// mallopt: map the Android knobs mimalloc can honor; return 0 for the rest.
// ---------------------------------------------------------------------------
int mi_mallopt(int param, int value) {
  switch (param) {
    case M_DECAY_TIME: {
      // mimalloc returns freed memory to the OS after mi_option_purge_delay
      // milliseconds. Map the Android tri-state (-1 disable / 0 immediate /
      // 1 default) onto a delay, matching the jemalloc wrapper's intent.
      long delay_ms;
      if (value < 0) {
        delay_ms = -1;    // never purge on a timer
      } else if (value == 0) {
        delay_ms = 0;     // purge eagerly
      } else {
        delay_ms = 1000;  // 1s default
      }
      mi_option_set(mi_option_purge_delay, delay_ms);
      return 1;
    }
    case M_PURGE:
    case M_PURGE_ALL:
      // Force-return as much memory to the OS as possible.
      mi_collect(/*force=*/true);
      return 1;
    case M_LOG_STATS:
      // Prints to mimalloc's default output; out must be NULL in v2.x.
      mi_stats_print(nullptr);
      return 1;
    default:
      return 0;
  }
}

// ---------------------------------------------------------------------------
// malloc_info: emit a minimal, well-formed XML document. mimalloc does not
// expose a per-bin breakdown through a stable API, so report process-level
// commit/RSS figures. Matches the <malloc version="..."> envelope bionic
// tooling expects.
// ---------------------------------------------------------------------------
int mi_malloc_info(int options, FILE* fp) {
  if (options != 0) {
    errno = EINVAL;
    return -1;
  }

  fflush(fp);
  int fd = fileno(fp);
  MallocXmlElem root(fd, "malloc", "version=\"mimalloc-1\"");

  size_t elapsed_msecs, user_msecs, system_msecs;
  size_t current_rss, peak_rss, current_commit, peak_commit, page_faults;
  mi_process_info(&elapsed_msecs, &user_msecs, &system_msecs, &current_rss, &peak_rss,
                  &current_commit, &peak_commit, &page_faults);

  MallocXmlElem(fd, "current-commit").Contents("%zu", current_commit);
  MallocXmlElem(fd, "peak-commit").Contents("%zu", peak_commit);
  MallocXmlElem(fd, "current-rss").Contents("%zu", current_rss);
  MallocXmlElem(fd, "peak-rss").Contents("%zu", peak_rss);
  return 0;
}

// v2.5.0 moved mi_malloc_usable_size into alloc-override.c (MI_MALLOC_OVERRIDE-gated,
// which we do not enable). Provide it here mapping to the always-compiled mi_usable_size.
extern "C" size_t mi_malloc_usable_size(const void* p) mi_attr_noexcept { return mi_usable_size(p); }
