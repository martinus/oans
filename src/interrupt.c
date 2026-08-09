/*
 * interrupt.c
 *
 * Signal handling for a graceful, durable shutdown. See interrupt.h.
 */

#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "interrupt.h"

/*
 * The only thing the handler touches. sig_atomic_t because nothing else is
 * guaranteed to be written indivisibly with respect to a signal, and volatile
 * so the polling loops re-read it rather than hoisting it out.
 *
 * Readers are on other threads, which strictly speaking wants an atomic load;
 * in practice this is a single aligned int written once and read repeatedly,
 * and the worst a stale read can do is check one more file before stopping.
 */
static volatile sig_atomic_t caught_signal;

static void interrupt_handler(int signo)
{
	caught_signal = signo;
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
}

bool interrupted(void)
{
	return caught_signal != 0;
}

int interrupt_signo(void)
{
	return (int)caught_signal;
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
 * Test-hook state. Read on first tick rather than at install time so a test can
 * see the same defaults as production when the variables are absent.
 */
static unsigned long tick_files, tick_batches;
static unsigned long limit_files, limit_batches;
static int test_signal = SIGINT;
static bool hooks_read;

static void read_hooks(void)
{
	const char *sig = getenv("DUPEREMOVE_INTERRUPT_SIGNAL");
	const char *f = getenv("DUPEREMOVE_INTERRUPT_AFTER");
	const char *b = getenv("DUPEREMOVE_INTERRUPT_AFTER_BATCHES");

	hooks_read = true;
	if (f)
		limit_files = strtoul(f, NULL, 10);
	if (b)
		limit_batches = strtoul(b, NULL, 10);
	if (sig && (!strcmp(sig, "TERM") || !strcmp(sig, "SIGTERM")))
		test_signal = SIGTERM;
}

static void tick(unsigned long *count, unsigned long limit)
{
	if (!hooks_read)
		read_hooks();
	if (!limit || interrupted())
		return;
	if (++*count >= limit)
		raise(test_signal);
}

void interrupt_test_file_tick(void)
{
	tick(&tick_files, limit_files);
}

void interrupt_test_batch_tick(void)
{
	tick(&tick_batches, limit_batches);
}
