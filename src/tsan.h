/*
 * tsan.h - teach ThreadSanitizer about GLib's synchronization primitives.
 *
 * TSAN derives happens-before edges by instrumenting the code that performs the
 * synchronization. libglib-2.0 is a system library built *without* -fsanitize=
 * thread, and on Linux GMutex/GCond are implemented directly on futex syscalls
 * rather than on pthread_mutex (which TSAN intercepts), so TSAN sees no edge at
 * all across a g_mutex_lock()/g_mutex_unlock() pair. Every correctly-locked
 * critical section then looks like a data race: an unannotated oans scan
 * reported ~145 of them, and a *provably* correct two-thread GMutex program
 * reports one too, which is how this was pinned down.
 *
 * The fix is to publish the edges TSAN cannot see. __tsan_release(addr) on
 * unlock and __tsan_acquire(addr) on lock, keyed by the primitive's own address,
 * reproduce exactly the ordering the primitive already guarantees. Same idea for
 * the pool/queue handoffs, keyed by the item pointer so the edge is no wider
 * than the real one.
 *
 * This is force-included (-include) by the Makefile's SANITIZE=thread build, so
 * no call site changes. It pulls in <glib.h> itself first: the macros below
 * would otherwise mangle GLib's own prototypes, since -include puts this ahead
 * of everything. Outside a TSAN build the whole file is empty.
 *
 * Over-annotating hides real races, so keep the edges tight: annotate the
 * primitive that actually orders the accesses and nothing broader.
 */
#ifndef __OANS_TSAN_H__
#define __OANS_TSAN_H__

/*
 * GCC has no __has_feature, and a #if expression is expanded whole - guarding it
 * with defined(__has_feature) in the same expression does not save us, GCC still
 * chokes on the call. Define it away first, the portable spelling.
 */
#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)

#include <glib.h>
#include <sanitizer/tsan_interface.h>

/* --- GMutex ------------------------------------------------------------- */

static inline void oans_tsan_mutex_lock(GMutex *m)
{
	g_mutex_lock(m);
	__tsan_acquire(m);
}

static inline gboolean oans_tsan_mutex_trylock(GMutex *m)
{
	gboolean got = g_mutex_trylock(m);

	if (got)
		__tsan_acquire(m);
	return got;
}

static inline void oans_tsan_mutex_unlock(GMutex *m)
{
	__tsan_release(m);
	g_mutex_unlock(m);
}

/* --- GCond -------------------------------------------------------------- */

/*
 * g_cond_wait() drops the mutex, blocks, and re-takes it, so mirror that:
 * release before handing the mutex back, acquire once we hold it again. The
 * condition variable itself carries the signaller -> waiter edge.
 */
static inline void oans_tsan_cond_wait(GCond *c, GMutex *m)
{
	__tsan_release(m);
	g_cond_wait(c, m);
	__tsan_acquire(m);
	__tsan_acquire(c);
}

static inline void oans_tsan_cond_signal(GCond *c)
{
	__tsan_release(c);
	g_cond_signal(c);
}

static inline void oans_tsan_cond_broadcast(GCond *c)
{
	__tsan_release(c);
	g_cond_broadcast(c);
}

/* --- Work handoff: GThreadPool and GAsyncQueue --------------------------- */

/*
 * Keyed on the item, not the pool/queue: a worker only needs to see what the
 * thread that handed it *that* item wrote. Keying on the container instead
 * would order every push against every pop and could bury a real race.
 * The matching acquire happens in the worker/popper via the pop wrapper and
 * oans_tsan_work_acquire().
 */
static inline void oans_tsan_pool_push(GThreadPool *pool, gpointer data,
				       GError **error)
{
	__tsan_release(data);
	g_thread_pool_push(pool, data, error);
}

static inline void oans_tsan_queue_push(GAsyncQueue *q, gpointer data)
{
	__tsan_release(data);
	g_async_queue_push(q, data);
}

static inline gpointer oans_tsan_queue_pop(GAsyncQueue *q)
{
	gpointer data = g_async_queue_pop(q);

	if (data)
		__tsan_acquire(data);
	return data;
}

/*
 * A GThreadPool worker is entered by GLib itself, so there is no pop call to
 * wrap - the worker function calls this on its own argument instead.
 */
static inline void oans_tsan_work_acquire(void *item)
{
	if (item)
		__tsan_acquire(item);
}

#define g_mutex_lock(m)			oans_tsan_mutex_lock(m)
#define g_mutex_trylock(m)		oans_tsan_mutex_trylock(m)
#define g_mutex_unlock(m)		oans_tsan_mutex_unlock(m)
#define g_cond_wait(c, m)		oans_tsan_cond_wait(c, m)
#define g_cond_signal(c)		oans_tsan_cond_signal(c)
#define g_cond_broadcast(c)		oans_tsan_cond_broadcast(c)
#define g_thread_pool_push(p, d, e)	oans_tsan_pool_push(p, d, e)
#define g_async_queue_push(q, d)	oans_tsan_queue_push(q, d)
#define g_async_queue_pop(q)		oans_tsan_queue_pop(q)

#else	/* not a TSAN build: nothing to annotate */

static inline void oans_tsan_work_acquire(void *item) { (void)item; }

#endif
#endif	/* __OANS_TSAN_H__ */
