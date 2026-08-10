#ifndef	__INTERRUPT_H__
#define	__INTERRUPT_H__

#include <stdbool.h>

/*
 * Graceful shutdown on SIGINT/SIGTERM (#201).
 *
 * Durability during a scan rides on the batched writer, which commits every
 * COMMIT_INTERVAL_SEC (10 s). Before this, a signal killed the process outright
 * and threw the open batch away, so a run interrupted more often than that made
 * no progress at all - and `systemctl stop oans@...`, the scheduled NAS case,
 * is exactly such a signal.
 *
 * The scheme is: the handler only raises a flag, and every loop that owns state
 * checks it and unwinds through its ordinary exit path. filescan_free() already
 * flushes the write batch on the way out, so nothing new has to be made
 * durable - the loops just have to *reach* it.
 *
 * The handler is installed with SA_RESTART (so a syscall interrupted mid-read
 * resumes rather than failing) and SA_RESETHAND, which puts the default action
 * back on delivery: a second Ctrl-C therefore kills at once, as a user
 * hammering it expects, without the handler having to do anything.
 */
void interrupt_install(void);

/* True once SIGINT or SIGTERM has been seen. Safe from any thread. */
bool interrupted(void);

/*
 * The signal that stopped the run, or 0. Used for the 128+signum exit status,
 * which is what a shell reports for a signalled child - so a wrapper script
 * sees "interrupted", not a distinct oans failure code.
 */
int interrupt_signo(void);

/*
 * Tell the user once that the run is winding down, so the pause between the
 * signal and the exit does not look like a hang. Idempotent, and safe to call
 * from any loop that notices the flag; goes through the progress-block routing
 * like every other message.
 */
void interrupt_report(void);

/*
 * Test hooks. A signal raced against a scan is not reproducible: sizing a tree
 * so that a `sleep N; kill` reliably lands mid-run means writing gigabytes in
 * setUp, and the test still turns into a coin flip on faster storage. These
 * raise the *real* signal - same handler, same unwinding, same exit status - at
 * a point the test names, exactly as DUPEREMOVE_CHECKPOINT_STOP stands in for
 * the kill that #159 exists to survive.
 *
 *   DUPEREMOVE_INTERRUPT_AFTER=N          after N files have finished hashing
 *   DUPEREMOVE_INTERRUPT_AFTER_BATCHES=N  after N dedupe generation batches
 *   DUPEREMOVE_INTERRUPT_SIGNAL=TERM      raise SIGTERM instead of SIGINT
 *
 * Counting *hashed* files rather than queued ones is what makes the durability
 * assertion exact: when the signal is raised, N files have already been written
 * to the open batch, so the shutdown either commits them or does not. Counting
 * queued files instead left the pool free to have finished none of them, which
 * is what a loaded CI runner did.
 *
 * The file counter is ticked by the csum workers, so it is atomic; the batch
 * counter belongs to the single dedupe producer. Unset - the normal case - both
 * cost one predictable branch.
 */
void interrupt_test_file_tick(void);
void interrupt_test_batch_tick(void);

#endif	/* __INTERRUPT_H__ */
