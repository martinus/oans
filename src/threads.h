/*
 * threads.h
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */

#ifndef	__THREADS_H__
#define	__THREADS_H__

#include <stdlib.h>
#include <stdio.h>

#include "opt.h"
#include "glib.h"

/*
 * The pool's worker signature. Typed rather than void *, so passing a function
 * whose parameters differ is a compile error: pool_trampoline() calls through
 * this exact type, and calling a function through a mismatched pointer is UB
 * that only clang's -fsanitize=function catches, at runtime, in a suite run.
 */
typedef void (*threads_pool_worker)(void *item, void *arg);

struct threads_cleanup_item {
	void (*function)(void *ptr);
	void *ptr;
};

struct threads_pool {
	GThreadPool *pool;
	/*
	 * Address only: the happens-before token workers publish on when they
	 * finish, collected in free_pool(). Deliberately not `pool`, which that
	 * teardown clears while a worker can still be in its tail. See tsan.h.
	 */
	char tsan_token;
	struct threads_cleanup_item** items;
	unsigned int item_count;
	GMutex mutex; /* Protect the cleanup items operations */

	/*
	 * Pushed-but-unfinished work, so a caller can wait for the pool to go
	 * quiet *without* tearing it down (free_pool would, but the pool outlives
	 * any single batch of work). The real worker runs behind a trampoline
	 * that drops the count, which is why setup_pool stashes it here.
	 */
	GCond idle_cond;
	unsigned int outstanding;
	threads_pool_worker worker;
	void *worker_arg;
};

void setup_pool(struct threads_pool *pool, threads_pool_worker function,
		void *arg, unsigned int max_threads);
void register_cleanup(struct threads_pool *pool, void *function, void *ptr);
void free_pool(struct threads_pool *pool);

/*
 * Queue one item. Use this rather than g_thread_pool_push() on pool->pool
 * directly: it is what keeps the outstanding count - and therefore
 * threads_pool_wait_idle() - honest.
 */
void threads_pool_push(struct threads_pool *pool, void *item, GError **err);

/*
 * Block until every item pushed so far has finished. Callers that hand workers
 * pointers into memory they are about to release MUST call this first - the
 * pool keeps running after the work is queued, and nothing else waits for it.
 */
void threads_pool_wait_idle(struct threads_pool *pool);

/* True when nothing is in flight right now. For assertions; a caller that needs
 * the pool to *become* idle must use threads_pool_wait_idle(). */
bool threads_pool_is_idle(struct threads_pool *pool);
#endif	/* __THREADS_H__ */
