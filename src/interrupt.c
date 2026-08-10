/*
 * interrupt.c
 *
 * Signal handling for a graceful, durable shutdown. See interrupt.h.
 */

#include <signal.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "interrupt.h"

/*
 * The only thing the handler touches.
 *
 * A lock-free atomic rather than the traditional `volatile sig_atomic_t`,
 * because the readers are on *other threads*: the handler runs on whichever
 * thread took the signal, and the csum workers and walkers poll from theirs.
 * `volatile` orders nothing across threads, so that is a data race however
 * benign it looks - ThreadSanitizer flags it, and is right to. C11 lets a
 * signal handler touch a lock-free atomic object, which is what makes this
 * both race-free and async-signal-safe; the static assertion below is what
 * keeps that true.
 *
 * Relaxed on both sides: there is nothing to order against. The flag carries
 * no data, and the worst a stale read can do is check one more file before
 * stopping.
 */
static _Atomic int caught_signal;
static_assert(ATOMIC_INT_LOCK_FREE == 2,
	      "the signal handler needs a lock-free atomic");

static void read_hooks(void);

static void interrupt_handler(int signo)
{
	atomic_store_explicit(&caught_signal, signo, memory_order_relaxed);
}

void interrupt_install(void)
{
	struct sigaction sa;
	int i;
	static const int signals[] = { SIGINT, SIGTERM };

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = interrupt_handler;
	sigemptyset(&sa.sa_mask);
	/*
	 * SA_RESTART: a walker blocked in read()/readdir() resumes instead of
	 * failing with EINTR, so the shutdown is driven by the flag checks alone
	 * and no error path has to learn about signals.
	 *
	 * SA_RESETHAND: the second one kills. Per-signal, so SIGINT then SIGTERM
	 * is still handled twice - the case that matters is a user pressing
	 * Ctrl-C again because the flush is taking longer than they expected.
	 */
	sa.sa_flags = SA_RESTART | SA_RESETHAND;

	for (i = 0; i < (int)(sizeof(signals) / sizeof(signals[0])); i++)
		sigaction(signals[i], &sa, NULL);

	read_hooks();
}

bool interrupted(void)
{
	return atomic_load_explicit(&caught_signal, memory_order_relaxed) != 0;
}

int interrupt_signo(void)
{
	return atomic_load_explicit(&caught_signal, memory_order_relaxed);
}

void interrupt_report(void)
{
	static _Atomic bool reported;

	if (!interrupted() || atomic_exchange(&reported, true))
		return;

	eprintf("Interrupted by %s - finishing the work in flight and "
		"committing what has been hashed. Signal again to abort.\n",
		interrupt_signo() == SIGINT ? "SIGINT" : "SIGTERM");
}

/*
 * Test-hook state, read once by interrupt_install() - on the main thread,
 * before any worker exists, so the ticks below only ever read it. Reading it
 * lazily on the first tick was a data race: the csum workers tick
 * concurrently, so whichever got there first wrote these while the others read
 * them.
 */
static _Atomic unsigned long tick_files;
static unsigned long tick_batches;
static unsigned long limit_files, limit_batches;
static int test_signal = SIGINT;

static void read_hooks(void)
{
	const char *sig = getenv("DUPEREMOVE_INTERRUPT_SIGNAL");
	const char *f = getenv("DUPEREMOVE_INTERRUPT_AFTER");
	const char *b = getenv("DUPEREMOVE_INTERRUPT_AFTER_BATCHES");

	if (f)
		limit_files = strtoul(f, NULL, 10);
	if (b)
		limit_batches = strtoul(b, NULL, 10);
	if (sig && (!strcmp(sig, "TERM") || !strcmp(sig, "SIGTERM")))
		test_signal = SIGTERM;
}

void interrupt_test_file_tick(void)
{
	if (!limit_files || interrupted())
		return;
	/*
	 * Ticked by the csum workers, so atomic - unlike the batch counter,
	 * which the single dedupe producer owns.
	 *
	 * Exactly the thread that crosses the limit raises, hence == and not
	 * >=: with SA_RESETHAND the disposition is back to the default by the
	 * time a second raise() lands, so two workers crossing together would
	 * kill the process outright - the very thing being tested against.
	 */
	if (atomic_fetch_add(&tick_files, 1) + 1 == limit_files)
		raise(test_signal);
}

void interrupt_test_batch_tick(void)
{
	if (!limit_batches || interrupted())
		return;
	if (++tick_batches >= limit_batches)
		raise(test_signal);
}
