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

#include "threads.h"
#include "debug.h"

/*
 * Every item runs through here so the outstanding count is dropped no matter
 * how the real worker returns. GLib hands us the pool as the GFunc's user_data
 * (see setup_pool), and the caller's original arg travels in worker_arg.
 */
static void pool_trampoline(gpointer item, gpointer user_data)
{
	struct threads_pool *pool = user_data;

	pool->worker(item, pool->worker_arg);

	g_mutex_lock(&pool->mutex);
	pool->outstanding--;
	if (pool->outstanding == 0)
		g_cond_broadcast(&pool->idle_cond);
	g_mutex_unlock(&pool->mutex);
}

void setup_pool(struct threads_pool *pool, threads_pool_worker function,
		void *arg, unsigned int max_threads)
{
	GError *err = NULL;

	pool->item_count = 0;
	pool->items = NULL;
	pool->outstanding = 0;
	pool->worker = function;
	pool->worker_arg = arg;
	g_mutex_init(&pool->mutex);
	g_cond_init(&pool->idle_cond);
	pool->pool = g_thread_pool_new(pool_trampoline, pool, max_threads, FALSE,
					&err);
	if (err != NULL) {
		eprintf("Unable to create thread pool: %s\n", err->message);
		g_error_free(err);
		pool->pool = NULL;
	}
}

void threads_pool_push(struct threads_pool *pool, void *item, GError **err)
{
	/* Count it before it can run: a worker may finish before push returns. */
	g_mutex_lock(&pool->mutex);
	pool->outstanding++;
	g_mutex_unlock(&pool->mutex);

	g_thread_pool_push(pool->pool, item, err);

	if (err && *err) {
		/* Never queued, so nothing will ever decrement it. */
		g_mutex_lock(&pool->mutex);
		pool->outstanding--;
		if (pool->outstanding == 0)
			g_cond_broadcast(&pool->idle_cond);
		g_mutex_unlock(&pool->mutex);
	}
}

/*
 * Snapshot of "no work in flight". Callers use this to assert a lifetime
 * invariant (see extents_search_idle), not to decide whether to wait - for
 * that, use threads_pool_wait_idle(), which cannot race.
 */
bool threads_pool_is_idle(struct threads_pool *pool)
{
	bool idle;

	g_mutex_lock(&pool->mutex);
	idle = pool->outstanding == 0;
	g_mutex_unlock(&pool->mutex);

	return idle;
}

void threads_pool_wait_idle(struct threads_pool *pool)
{
	g_mutex_lock(&pool->mutex);
	while (pool->outstanding > 0)
		g_cond_wait(&pool->idle_cond, &pool->mutex);
	g_mutex_unlock(&pool->mutex);
}

void register_cleanup(struct threads_pool *pool, void *function, void *ptr)
{
	struct threads_cleanup_item *item;
	item = calloc(1, sizeof(struct threads_cleanup_item));
	item->ptr = ptr;
	item->function = function;

	g_mutex_lock(&pool->mutex);
	pool->items = realloc(pool->items, (pool->item_count + 1) * sizeof(struct threads_cleanup_item*));
	pool->items[pool->item_count] = item;
	pool->item_count += 1;
	g_mutex_unlock(&pool->mutex);
}

void free_pool(struct threads_pool *pool)
{
	g_thread_pool_free(pool->pool, FALSE, TRUE);

	/*
	 * The wait above means no worker can still be registering, but take the
	 * mutex anyway: it is the same lock register_cleanup() writes under, so
	 * the ordering is stated in the code rather than left to GLib internals
	 * (which a race detector cannot see through).
	 */
	g_mutex_lock(&pool->mutex);
	for (unsigned int i = 0; i < pool->item_count; i++) {
		struct threads_cleanup_item *item;
		item = pool->items[i];
		item->function(item->ptr);
		free(item);
	}

	if (pool->items)
		free(pool->items);

	pool->items = NULL;
	pool->item_count = 0;
	g_mutex_unlock(&pool->mutex);

	pool->pool = NULL;
}
