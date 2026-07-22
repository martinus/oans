#ifndef	__PROGRESS_H__
#define	__PROGRESS_H__

#include <stdint.h>

#include "glib.h"

enum pscan_thread_status {
	thread_idle,
	thread_scanning,
	thread_waiting_lock,
	thread_committing,
	thread_deduping,
};

struct pscan_thread {
	/* Thread id owning this struct */
	pid_t				tid;

	/* Tracks data for the entire thread lifetime */
	uint64_t			total_scanned_files;
	uint64_t			total_scanned_bytes;

	/* Tracks data for the file currently being processed */
	uint64_t			file_scanned_bytes;
	uint64_t			file_total_bytes;
	char				file_path[PATH_MAX + 1];

	enum pscan_thread_status	status;
};

struct pscan_global {
	uint64_t		total_files_count;
	uint64_t		total_bytes_count;
	uint64_t		files_examined;	/* visited during listing */
	bool			listing_completed;

	/* Each thread tracks its own progress separately */
	GMutex			mutex;
	unsigned int		thread_count;
	struct pscan_thread	**threads;
};

void pscan_finish_listing(void);

/* Used to increment the global todo list */
void pscan_set_progress(uint64_t added_files, uint64_t added_bytes);

/* Count one file visited by the listing walk (scanned or skipped as up-to-date). */
void pscan_examined(void);
uint64_t pscan_files_scanned(void);

/* Used by each scan threads to grab its own struct pscan_thread */
struct pscan_thread *pscan_register_thread(pid_t tid);

/*
 * Setup the pty and start the progress thread
 * The thread will run until the scan is done, that is:
 * - the listing is completed - pscan_finish_listing() has been called
 * - the sum of all threads progresses equals to the global totals
 */
void pscan_run(void);

/*
 * Wait for the progress thread to finish
 * Also cleanup per-thread progresses and print the global totals
 */
void pscan_join(void);

/*
 * Per-work-item reset for the churning dedupe pool: finish the current file's
 * accounting (total_scanned_files++, total_scanned_bytes fed up to the file
 * size in case it shrank) and park the slot idle. Equivalent to
 * pscan_finish_file() followed by pscan_slot_idle(); persistent csum workers
 * use those two directly instead so they don't flash idle between files.
 */
void pscan_reset_thread(struct pscan_thread **progress);

/*
 * For a persistently-held slot (one kept by a long-lived worker across many
 * files, rather than re-claimed per file): pscan_finish_file() does the per-file
 * byte/count accounting without going idle, so no "idle" flashes between files;
 * pscan_slot_idle() parks the slot when the worker finally runs out of work.
 */
void pscan_finish_file(struct pscan_thread **progress);
void pscan_slot_idle(struct pscan_thread *slot);

bool is_progress_printer_running(void);

/*
 * The progress thread overwrites its area.
 * This function is used to write something before that area
 */
void pscan_printf(char *fmt, ...);

/*
 * Dedupe-phase status. Reuses the same reserved screen area, per-thread lines
 * and totals block as the scan, so both phases look alike. The counters are
 * always maintained (they feed the final summary); the live display only runs
 * on a tty and outside -q/-v.
 *
 * The displayed total is max(estimated, queued-so-far) and the percentage is
 * capped at 99% - the estimate is deliberately fuzzy (see
 * dbfile_count_dupe_groups()), and a bar pinned at 100% while work continues
 * is worse than one that finishes from 99%.
 */
void pdedupe_begin(uint64_t estimated_groups, unsigned int batches);
void pdedupe_end(void);

/* Starting duplicate-batch cur (1-based) of the batches passed to begin. */
void pdedupe_set_batch(unsigned int cur);

/*
 * What the phase is doing right now ("loading identical files", ...): shown on
 * the status line so the pauses between dedupes are explained. Must be a
 * string literal / static string.
 */
void pdedupe_set_activity(const char *activity);

/* A group was pushed to / finished by the dedupe pool. */
void pdedupe_add_queued(uint64_t ngroups);
void pdedupe_group_done(uint64_t reclaimed_bytes, uint64_t net_shared_bytes);

/*
 * Claim a per-thread display line for one unit of work (file scan / dedupe
 * group) and mark it with `status`; fill in file_path / file_total_bytes /
 * file_scanned_bytes afterwards. Release it with pscan_reset_thread(). Claim
 * per work item, not per OS thread: pool threads get reaped and respawned,
 * which would strand dead threads' lines and grow the display unboundedly.
 */
struct pscan_thread *pscan_claim_slot(pid_t tid,
				      enum pscan_thread_status status);

/*
 * Read back the accumulated totals (for the end-of-run summary). reclaimed is
 * the honest disk-freed amount (kernel-deduped bytes); net_shared is the fiemap
 * "net change in shared extents" diagnostic and may be NULL if not needed.
 */
void pdedupe_counters(uint64_t *groups, uint64_t *reclaimed, uint64_t *net_shared);

/*
 * Start the "extent search" progress thread
 * The thread will run until the search is done, that is when we
 * processed all filerecs
 */
void psearch_run(uint64_t num_filerecs);

/*
 * Wait for the progress thread to finish
 */
void psearch_join(void);

/*
 * extent search: update the number of processed filerecs
 */
void psearch_update_processed_count(unsigned int processed);

#endif	/* __PROGRESS_H__ */
