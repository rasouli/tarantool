#ifndef TARANTOOL_BOX_ALLOCATOR_H_INCLUDED
#define TARANTOOL_BOX_ALLOCATOR_H_INCLUDED
/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include <small/small.h>
#include <trivia/util.h>

#include "memtx_engine.h"
#include "tuple.h"
#include "system_allocator.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct allocator_stats {
	size_t used;
	size_t total;
};

#define noop_one_arg(a)
#define noop_two_arg(a, b)

#define DECLARE_MEMTX_ALLOCATOR_STATS(type)						\
static inline void									\
type##_##allocator_stats(struct memtx_engine *memtx, struct allocator_stats *stats,	\
		      mempool_stats_cb stats_cb, void *cb_ctx)				\
{											\
	struct type##_##stats data_stats;						\
	type##_##stats(memtx->alloc, &data_stats, stats_cb, cb_ctx);			\
	stats->used = data_stats.used;							\
	stats->total = data_stats.total;						\
}
DECLARE_MEMTX_ALLOCATOR_STATS(small)
DECLARE_MEMTX_ALLOCATOR_STATS(system)
/**
 * Global abstract method to get allocation statistic
 */
typedef void (*global_alloc_stats)(struct memtx_engine *memtx, struct allocator_stats *stats,
			mempool_stats_cb stats_cb, void *cb_ctx);
extern global_alloc_stats memtx_allocator_stats;

#define DECLARE_MEMTX_ALLOCATOR_ENTER_DELAYED_FREE_MODE(type, TYPE)			\
static inline void									\
type##_##allocator_enter_delayed_free_mode(struct memtx_engine *memtx)			\
{											\
	return type##_##alloc_setopt(memtx->alloc, TYPE##_##DELAYED_FREE_MODE, true);	\
}
DECLARE_MEMTX_ALLOCATOR_ENTER_DELAYED_FREE_MODE(small, SMALL)
DECLARE_MEMTX_ALLOCATOR_ENTER_DELAYED_FREE_MODE(system, SYSTEM)
/**
 * Global abstact method to enter delayed free mode
 */
typedef void (*global_enter_delayed_free_mode)(struct memtx_engine *memtx);

#define DECLARE_MEMTX_ALLOCATOR_LEAVE_DELAYED_FREE_MODE(type, TYPE)			\
static inline void									\
type##_##allocator_leave_delayed_free_mode(struct memtx_engine *memtx)			\
{											\
	return type##_##alloc_setopt(memtx->alloc, TYPE##_##DELAYED_FREE_MODE, false);	\
}
DECLARE_MEMTX_ALLOCATOR_LEAVE_DELAYED_FREE_MODE(small, SMALL)
DECLARE_MEMTX_ALLOCATOR_LEAVE_DELAYED_FREE_MODE(system, SYSTEM)
/**
 * Global abstact method to leave delayed free mode
 */
typedef void (*global_leave_delayed_free_mode)(struct memtx_engine *memtx);

#define DECLARE_MEMTX_ALLOCATOR_INIT(type, SLAB_CACHE_CREATE, param)			\
static inline void 									\
type##_##allocator_init(struct memtx_engine *memtx, uint32_t objsize_min,		\
			float alloc_factor)						\
{											\
	SLAB_CACHE_CREATE(&memtx->slab_cache, &memtx->arena);				\
	type##_##alloc_create(memtx->alloc, &memtx->param, objsize_min,			\
		alloc_factor);								\
}
DECLARE_MEMTX_ALLOCATOR_INIT(small, slab_cache_create, slab_cache)
DECLARE_MEMTX_ALLOCATOR_INIT(system, noop_two_arg, quota)
/**
 * Global abstact method for allocator initialization
 */
typedef void (*global_allocator_init)(struct memtx_engine *memtx,
			uint32_t objsize_min, float alloc_factor);

#define DECLARE_MEMTX_ALLOCATOR_DESTROY(type, SLAB_CACHE_DESTROY)			\
static inline void 									\
type##_##allocator_destroy(struct memtx_engine *memtx)					\
{											\
	type##_##alloc_destroy(memtx->alloc);						\
	SLAB_CACHE_DESTROY(&memtx->slab_cache);						\
	tuple_arena_destroy(&memtx->arena);						\
}
DECLARE_MEMTX_ALLOCATOR_DESTROY(small, slab_cache_destroy)
DECLARE_MEMTX_ALLOCATOR_DESTROY(system, noop_one_arg)
/**
 * Global abstract method to allocator destroy
 */
typedef void (*global_allocator_destroy)(struct memtx_engine *memtx);

#define DECLARE_MEMTX_SLAB_CACHE_CHECK(type, SLAB_CACHE_CHECK)				\
static inline void									\
type##_##slab_cache_check(struct memtx_engine *memtx)					\
{											\
	SLAB_CACHE_CHECK(((struct type##_alloc *)(memtx->alloc))->cache);		\
	(void)memtx;									\
}
DECLARE_MEMTX_SLAB_CACHE_CHECK(small, slab_cache_check)
DECLARE_MEMTX_SLAB_CACHE_CHECK(system, noop_one_arg)
/**
 * Global abstract method to slab cache check
 */
typedef void (*global_slab_cache_check)(struct memtx_engine *memtx);
extern global_slab_cache_check memtx_slab_cache_check;

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */


#endif /* TARANTOOL_BOX_ALLOCATOR_H_INCLUDED */