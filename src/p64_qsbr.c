//Copyright (c) 2018, ARM Limited. All rights reserved.
//
//SPDX-License-Identifier:        BSD-3-Clause

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef PRIVATE
#include "p64_qsbr.h"
#define PUBLIC
#else
typedef struct p64_qsbrdomain p64_qsbrdomain_t;
#define PUBLIC static inline
#endif
#include "build_config.h"
#include "os_abstraction.h"

#include "arch.h"
#include "lockfree.h"
#include "common.h"
#include "thr_idx.h"

#ifndef PRIVATE
static void
eprintf_not_registered(void)
{
    fprintf(stderr, "qsbr: p64_qsbr_register() not called\n");
    fflush(stderr);
    abort();
}
#else
#define eprintf_not_registered(x)
#endif

struct p64_qsbrdomain
{
    uint64_t current;//Current interval
    uint32_t maxobjs;
    uint32_t high_wm;//High watermark of thread index
    uint64_t intervals[MAXTHREADS] ALIGNED(CACHE_LINE);//Each thread's last quiescent interval
};

//Value larger than all possible intervals
#define INFINITE (~(uint64_t)0)

PUBLIC p64_qsbrdomain_t *
p64_qsbr_alloc(uint32_t maxobjs)
{
    p64_qsbrdomain_t *qsbr = p64_malloc(sizeof(p64_qsbrdomain_t), CACHE_LINE);
    if (qsbr != NULL)
    {
	qsbr->current = 0;
	qsbr->maxobjs = maxobjs;
	qsbr->high_wm = 0;
	for (uint32_t i = 0; i < MAXTHREADS; i++)
	{
	    qsbr->intervals[i] = INFINITE;
	}
	return qsbr;
    }
    return NULL;
}

//Find the smallest value in the array
static uint64_t
find_min(const uint64_t vec[], uint32_t num)
{
    uint64_t min = INFINITE;
    for (uint32_t i = 0; i < num; i++)
    {
	uint64_t t = __atomic_load_n(&vec[i], __ATOMIC_ACQUIRE);
	if (t < min)
	{
	    min = t;
	}
    }
    return min;
}

PUBLIC void
p64_qsbr_free(p64_qsbrdomain_t *qsbr)
{
    uint64_t interval = find_min(qsbr->intervals, qsbr->high_wm);
    if (interval != INFINITE)
    {
	fprintf(stderr, "qsbr: Registered threads still present\n");
	fflush(stderr);
	abort();
    }
    p64_mfree(qsbr);
}

typedef void *userptr_t;

struct object
{
    userptr_t ptr;
    void (*cb)(userptr_t);
    uint64_t interval;
};

struct thread_state
{
    p64_qsbrdomain_t *qsbr;
    uint64_t interval;//Last seen interval
    uint32_t recur;//Acquire/release recursion level
    uint32_t idx;//Thread index
    //Removed but not yet reclaimed objects
    uint32_t nobjs;
    uint32_t maxobjs;
    struct object objs[];
} ALIGNED(CACHE_LINE);

static __thread struct thread_state *TS = NULL;

static struct thread_state *
alloc_ts(p64_qsbrdomain_t *qsbr)
{
    assert(TS == NULL);
    //Attempt to allocate a thread index
    int32_t idx = p64_idx_alloc();
    if (idx < 0)
    {
	fprintf(stderr, "qsbr: Too many registered threads\n");
	fflush(stderr);
	abort();
    }
    size_t nbytes = sizeof(struct thread_state) +
		    qsbr->maxobjs * sizeof(struct object);
    struct thread_state *ts = p64_malloc(nbytes, CACHE_LINE);
    if (ts == NULL)
    {
	perror("malloc"), exit(EXIT_FAILURE);
    }
    ts->qsbr = qsbr;
    ts->interval = INFINITE;
    ts->recur = 0;
    ts->idx = idx;
    ts->nobjs = 0;
    ts->maxobjs = qsbr->maxobjs;
    assert(qsbr->intervals[idx] == INFINITE);
    //Conditionally update high watermark of indexes
    lockfree_fetch_umax_4(&qsbr->high_wm, (uint32_t)idx + 1, __ATOMIC_RELAXED);
    return ts;
}

PUBLIC void
p64_qsbr_reactivate(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    uint64_t current = __atomic_load_n(&TS->qsbr->current, __ATOMIC_RELAXED);
    __atomic_store_n(&TS->qsbr->intervals[TS->idx], current, __ATOMIC_RELAXED);
    TS->interval = current;
    //Ensure our interval is observable before any reads are observed
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

PUBLIC void
p64_qsbr_register(p64_qsbrdomain_t *qsbr)
{
    if (UNLIKELY(TS == NULL))
    {
	TS = alloc_ts(qsbr);
    }
    p64_qsbr_reactivate();
}

PUBLIC void
p64_qsbr_deactivate(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    //Mark thread as inactive, no references kept
    __atomic_store_n(&TS->qsbr->intervals[TS->idx], INFINITE, __ATOMIC_RELEASE);
    TS->interval = INFINITE;
}

PUBLIC void
p64_qsbr_unregister(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    if (TS->nobjs != 0)
    {
	fprintf(stderr, "qsbr: Thread has %u unreclaimed objects\n", TS->nobjs);
	fflush(stderr);
	abort();
    }
    p64_qsbr_deactivate();
    p64_idx_free(TS->idx);
    p64_mfree(TS);
    TS = NULL;
}

static inline void
quiescent(void)
{
    p64_qsbrdomain_t *qsbr = TS->qsbr;
    uint64_t current = __atomic_load_n(&qsbr->current, __ATOMIC_RELAXED);
    if (current != TS->interval)
    {
	//Release order to contain all our previous access to shared objects
	__atomic_store_n(&qsbr->intervals[TS->idx], current, __ATOMIC_RELEASE);
	TS->interval = current;
    }
}

PUBLIC void
p64_qsbr_quiescent(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    if (UNLIKELY(TS->interval == INFINITE))
    {
	fprintf(stderr, "qsbr: Inactive thread calling p64_qsbr_quiescent()\n");
	fflush(stderr);
	abort();
    }
    quiescent();
}

PUBLIC void
p64_qsbr_acquire(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    if (UNLIKELY(TS->interval == INFINITE))
    {
	fprintf(stderr, "qsbr: Inactive thread calling p64_qsbr_acquire()\n");
	fflush(stderr);
	abort();
    }
    TS->recur++;
}

PUBLIC void
p64_qsbr_release(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    if (UNLIKELY(TS->recur == 0))
    {
	fprintf(stderr, "qsbr: mismatched call to p64_qsbr_release()\n");
	fflush(stderr);
	abort();
    }
    if (--TS->recur == 0)
    {
	quiescent();
    }
}

//Traverse all pending objects and reclaim those that have no references
static uint32_t
garbage_collect(void)
{
    uint32_t numthrs = __atomic_load_n(&TS->qsbr->high_wm, __ATOMIC_ACQUIRE);
    uint64_t interval = find_min(TS->qsbr->intervals, numthrs);
    //Traverse list of pending objects
    uint32_t nobjs = 0;
    for (uint32_t i = 0; i < TS->nobjs;)
    {
	struct object obj = TS->objs[i++];
	if (interval > obj.interval)
	{
	    //All threads have observed a later interval =>
	    //No thread has any reference to this object, reclaim it
	    obj.cb(obj.ptr);
	}
	else
	{
	    //Retired object still referenced, keep it in rlist
	    TS->objs[nobjs++] = obj;
	    //Keep the rest of the objects as well
	    while (i < TS->nobjs)
	    {
		assert(TS->objs[i].interval >= TS->objs[i - 1].interval);
		TS->objs[nobjs++] = TS->objs[i++];
	    }
	    break;
	}
    }
    //Some objects may remain in the list of retired objects
    TS->nobjs = nobjs;
    //Return number of remaining unreclaimed objects
    //Caller can compute number of available slots
    return nobjs;
}

//Retire an object
//If necessary, perform garbage collection on retired objects
PUBLIC bool
p64_qsbr_retire(void *ptr,
		void (*cb)(void *ptr))
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    if (UNLIKELY(TS->nobjs == TS->maxobjs))
    {
	if (garbage_collect() == TS->maxobjs)
	{
	    return false;//No space for object
	}
    }
    assert(TS->nobjs < TS->maxobjs);
    uint32_t i = TS->nobjs++;
    TS->objs[i].ptr = ptr;
    TS->objs[i].cb = cb;
    //Create a new interval
    //Release order to ensure removal is observable before new interval is
    //created and can be observed
    uint64_t previous = __atomic_fetch_add(&TS->qsbr->current,
					   1,
					   __ATOMIC_RELEASE);
    //Retired object belongs to previous interval
    TS->objs[i].interval = previous;
    //The object can be reclaimed when all threads have observed
    //the new interval
    return true;
}

PUBLIC uint32_t
p64_qsbr_reclaim(void)
{
    if (UNLIKELY(TS == NULL))
    {
	eprintf_not_registered();
    }
    if (TS->nobjs == 0)
    {
	//Nothing to reclaim
	return 0;
    }
    //Try to reclaim objects
    uint32_t nremaining = garbage_collect();
    return nremaining;
}

#undef eprintf_not_registered
