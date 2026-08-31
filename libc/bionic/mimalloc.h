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

#pragma once

// bionic is built with stl:none and -nostdlibinc; skip mimalloc's C++
// std::allocator adapter (it pulls <cstddef>/<type_traits>/<utility>).
#define MI_SKIP_CPP_ALLOCATOR
#include <mimalloc.h>
#include <malloc.h>  // For struct mallinfo.

// bionic's allocator dispatch calls Malloc(memalign) == mi_memalign. The
// historical memalign(3) contract rounds a non power-of-two boundary up to the
// next power of two (both glibc and dlmalloc do this), but mimalloc's
// mi_memalign requires a power-of-two alignment. Route the dispatch entry to a
// wrapper that rounds up first. (Mirrors the jemalloc wrapper.)
#define mi_memalign mi_memalign_round_up_boundary

// bionic's aligned_alloc must enforce that size is an integral multiple of
// alignment (C11); mimalloc's mi_aligned_alloc does not. Route through a
// wrapper that enforces it.
#define mi_aligned_alloc mi_aligned_alloc_wrapper

__BEGIN_DECLS

// These entry points are NOT provided by libmimalloc; they are implemented in
// mimalloc_wrapper.cpp to satisfy the bionic MallocDispatch table.
struct mallinfo mi_mallinfo();
int mi_malloc_iterate(uintptr_t base, size_t size,
                      void (*callback)(uintptr_t base, size_t size, void* arg), void* arg);
void mi_malloc_disable();
void mi_malloc_enable();
int mi_malloc_info(int options, FILE* fp);
int mi_mallopt(int param, int value);
void* mi_memalign_round_up_boundary(size_t boundary, size_t size);
void* mi_aligned_alloc_wrapper(size_t alignment, size_t size);

__END_DECLS
