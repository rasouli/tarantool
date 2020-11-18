#ifndef TARANTOOL_BOX_SYSTEM_ALLOCATOR_H_INCLUDED
#define TARANTOOL_BOX_SYSTEM_ALLOCATOR_H_INCLUDED
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
#include <stdlib.h>
#include <trivia/util.h>

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

#if (defined (__APPLE__) || defined (__MACH__))
#include <malloc/malloc.h>
static inline size_t
portable_malloc_usable_size(void *p)
{
	return malloc_size(p);
}
#elif (defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__bsdi__))
#include <malloc_np.h>
static inline size_t
portable_malloc_usable_size(void *p)
{
	return malloc_usable_size(p);
}
#elif defined(__linux__)
#include <malloc.h>
static inline size_t
portable_malloc_usable_size(void *p)
{
	return malloc_usable_size(p);
}
#else
#error "Undefined system type"
#endif


struct system_alloc {
	/**
	 * Bytes alloced by system allocator
	 */
	uint64_t used_bytes;
	/**
	 * Allocator quota
	 */
	struct quota *quota;
};

struct system_stats {
	size_t used;
	size_t total;
};

enum system_opt {
	SYSTEM_DELAYED_FREE_MODE
};

typedef int (*system_stats_cb)(const struct system_stats *stats,
				void *cb_ctx);

static inline void
system_alloc_setopt(struct system_alloc *alloc, enum system_opt opt, bool val)
{
	(void)alloc;
	(void)opt;
	(void)val;
}

static inline void
system_stats(struct system_alloc *alloc, struct system_stats *totals,
	mempool_stats_cb cb, void *cb_ctx)
{
	struct mempool_stats stats;
	memset(&stats, 0, sizeof(stats));
	totals->used =  pm_atomic_load(&alloc->used_bytes);
	totals->total = quota_total(alloc->quota);
	stats.totals.used = totals->used;
	stats.totals.total = totals->total;
	cb(&stats, cb_ctx);
}

static inline void
system_alloc_create(struct system_alloc *alloc, struct quota *quota,
	uint32_t objsize_min, float alloc_factor)
{
	alloc->used_bytes = 0;
	alloc->quota = quota;
	(void)objsize_min;
	(void)alloc_factor;
}

static inline void
system_alloc_destroy(MAYBE_UNUSED struct system_alloc *alloc)
{

}

static inline void
sysfree(struct system_alloc *alloc, void *ptr, MAYBE_UNUSED unsigned bytes)
{
	size_t size = portable_malloc_usable_size(ptr); /* return 0 if ptr is NULL */
	uint32_t s = size % QUOTA_UNIT_SIZE, units = size / QUOTA_UNIT_SIZE;
	size_t used_bytes =  pm_atomic_fetch_sub(&alloc->used_bytes, size);
	if (small_align(used_bytes, QUOTA_UNIT_SIZE) > small_align(used_bytes - s, QUOTA_UNIT_SIZE))
		units++;
	if (units > 0)
		quota_release(alloc->quota, units * QUOTA_UNIT_SIZE);
	free(ptr);
}

static inline void
sysfree_delayed(struct system_alloc *alloc, void *ptr, MAYBE_UNUSED unsigned bytes)
{
	return sysfree(alloc, ptr, bytes);
}

static inline void *
sysalloc(struct system_alloc *alloc, unsigned bytes)
{
	void *ptr = malloc(bytes);
	size_t size = portable_malloc_usable_size(ptr); /* return 0 if ptr is NULL */
	uint32_t s = size % QUOTA_UNIT_SIZE, units = size / QUOTA_UNIT_SIZE;
	while (ptr != NULL) {
		size_t used_bytes =  pm_atomic_load(&alloc->used_bytes);
		if (small_align(used_bytes, QUOTA_UNIT_SIZE) < small_align(used_bytes + s, QUOTA_UNIT_SIZE))
			units++;
		if (units > 0) {
			if (quota_use(alloc->quota, units * QUOTA_UNIT_SIZE) < 0) {
				free(ptr);
				return NULL;
			}
		}
		if (pm_atomic_compare_exchange_strong(&alloc->used_bytes, &used_bytes, used_bytes + size))
			break;
		if (units > 0)
			quota_release(alloc->quota, units * QUOTA_UNIT_SIZE);
	}	
	return ptr;
}

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_BOX_SYSTEM_ALLOCATOR_H_INCLUDED */