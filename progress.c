/*
 * progress.c
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
#include <sys/ioctl.h>
#include <stdatomic.h>

#include "debug.h"
#include "opt.h"
#include "util.h"
#include "progress.h"

/*
 * This implements a multi progressbar
 * To do this, it reserves n + 3 lines from the bottom
 * of the screen and save the position
 * Those lines will be used in the following way:
 * one line per thread (up to n lines), and then 3 lines for the
 * totals:
 * ### thread 1 progress
 * ### thread 2 progress
 * ###
 * ### thread n progress
 * ### 	Total bytes
 * ### 	Total files
 * ### 	Listing status
 *
 * n is derived from the maximum number of threads, not from the actual
 * number of thread. In such cases, empty lines will follow the "totals"
 * block.
 *
 * Every time the progress thread tries to print the progress, it:
 * - jump back to the saved position
 * - print one line for each running thread, cleaning the existing line
 * - print the totals (again, cleaning the existing line)
 *
 * A function pscan_printf() is provided to print data while the progress
 * thread is running
 * It will grab the lock and print the data before the "progress" area:
 * - jump back to the saved position
 * - print the data
 * - reserves n + 3 lines
 * - save the new position so that the data is not overwritten
 *
 * If stdout is not a tty, no asci code are printed, so this acts as
 * an append-only progressbar.
 */

struct pscan_global pscan = {};
static GThread *printer = NULL;
bool tty;
unsigned int w_col;

/* Sums of the per-thread stats */
static uint64_t files_scanned, bytes_scanned;

/*
 * Used to track the status of our search extents from blocks
 */
static _Atomic uint64_t search_total, search_processed;

/*
 * Dedupe-phase status. The counters accumulate whether or not the live
 * display runs (they feed the final summary). `running` gates the printer
 * thread; `phase` switches the shared screen area between scan and dedupe
 * rendering.
 */
static struct {
	bool			phase;		/* rendering dedupe, not scan */
	volatile int		running;
	gint64			start_us;	/* phase start, monotonic */

	_Atomic uint64_t	done;		/* groups finished */
	_Atomic uint64_t	queued;		/* groups pushed to the pool */
	uint64_t		estimate;	/* fuzzy total, see dbfile */
	unsigned int		batch, batches;
	const char		*activity;	/* static string */
	_Atomic uint64_t	reclaimed, kern;
} pdd;

#define s_save_pos() if (tty) printf("\33[s");
#define s_restore_pos() if (tty) printf("\33[u");
#define s_clear() if (tty) printf("\33[J");
#define s_printf(args...) do { if (tty) printf("\33[K"); printf(args); } while (0)

#define percent(val1, val2) ((double) val1 / (double) val2 * 100)

void pscan_finish_listing(void)
{
	pscan.listing_completed = true;
}

void pscan_set_progress(uint64_t added_files, uint64_t added_bytes)
{
	pscan.total_files_count += added_files;
	pscan.total_bytes_count += added_bytes;
}

void pscan_examined(void)
{
	pscan.files_examined++;	/* listing is single-threaded; plain ++ is fine */
}

#define BUF_LEN 10*1024
static void print_thread_progress(struct pscan_thread *tprogress)
{
	char buf[BUF_LEN];

	switch (tprogress->status) {
	case thread_idle:
		snprintf(buf, BUF_LEN, "[%u] idle", tprogress->tid);
		break;
	case thread_scanning:
		snprintf(buf, BUF_LEN, "[%u] %-20s%s: %s/%s (%05.2f%%)",
			tprogress->tid,
			"checksumming:",
			tprogress->file_path,
			pretty_size(tprogress->file_scanned_bytes),
			pretty_size(tprogress->file_total_bytes),
			percent(tprogress->file_scanned_bytes, tprogress->file_total_bytes));
		break;
	case thread_waiting_lock:
		snprintf(buf, BUF_LEN, "[%u] %-20s%s (size: %s)",
			tprogress->tid,
			"waiting for lock:",
			tprogress->file_path,
			pretty_size(tprogress->file_total_bytes));
		break;
	case thread_committing:
		snprintf(buf, BUF_LEN, "[%u] %-20s%s (size: %s)",
			tprogress->tid,
			"committing:",
			tprogress->file_path,
			pretty_size(tprogress->file_total_bytes));
		break;
	case thread_deduping:
		snprintf(buf, BUF_LEN, "[%u] %-20s%s: %s/%s (%05.2f%%)",
			tprogress->tid,
			"deduping:",
			tprogress->file_path,
			pretty_size(tprogress->file_scanned_bytes),
			pretty_size(tprogress->file_total_bytes),
			percent(tprogress->file_scanned_bytes, tprogress->file_total_bytes));
		break;
	}

	/* Truncate the output to keep at most one line per thread */
	s_printf("%.*s\n", w_col, buf);
}

static void print_scan_progress(void)
{
	uint64_t tf = pscan.total_files_count;
	uint64_t tb = pscan.total_bytes_count;
	double elapsed = elapsed_seconds();

	if (!pscan.listing_completed) {
		/*
		 * During discovery the total isn't known yet (and most files may
		 * be up to date, so total_files_count stays low). Show what we do
		 * know - files walked and how many need (re)hashing - rather than
		 * a misleading 0/0.
		 */
		s_printf("\tListing files: %lu examined\n", pscan.files_examined);
		s_printf("\t%lu need hashing\n", tf);
		s_printf("\tFile listing: in progress\n");
		return;
	}

	s_printf("\tFiles scanned: %lu/%lu (%05.2f%%)\n",
	      files_scanned, tf, tf ? (double)files_scanned / tf * 100 : 100.0);
	s_printf("\tBytes scanned: %s/%s (%05.2f%%)",
	      pretty_size(bytes_scanned), pretty_size(tb),
	      tb ? (double)bytes_scanned / tb * 100 : 100.0);
	if (bytes_scanned && elapsed > 1.0) {
		double rate = bytes_scanned / elapsed;

		printf(" · %s/s", pretty_size((uint64_t)rate));
		if (bytes_scanned < tb)
			printf(" · ETA ~%s",
			       human_duration((tb - bytes_scanned) / rate));
	}
	printf("\n");
	s_printf("\tFile listing: completed\n");
}

static void print_dedupe_progress(void)
{
	uint64_t done = pdd.done;
	uint64_t queued = pdd.queued;
	uint64_t total = pdd.estimate > queued ? pdd.estimate : queued;
	uint64_t sd = search_processed, st = search_total;
	double elapsed = (g_get_monotonic_time() - pdd.start_us) / 1e6;
	unsigned int pct = 0;

	/*
	 * The total is fuzzy until the very last batch is queued (and the
	 * whole-file pass shrinks the extent-group count as it goes), so mark
	 * it "~" and never claim 100% while the phase is still running.
	 */
	if (total) {
		pct = (unsigned int)(100.0 * done / total);
		if (pct > 99)
			pct = 99;
	}

	s_printf("\tGroups deduped: %lu/~%lu (%u%%)", done, total, pct);
	if (pdd.batches > 1)
		printf(" · batch %u/%u", pdd.batch, pdd.batches);
	printf("\n");

	s_printf("\tSpace reclaimed: %s · kernel verified: %s\n",
		 pretty_size(pdd.reclaimed), pretty_size(pdd.kern));

	s_printf("\tStatus: %s", pdd.activity ? pdd.activity : "working");
	if (st)
		printf(" (%lu/%lu files)", sd, st);
	if (elapsed > 1.0)
		printf(" · elapsed %s", human_duration(elapsed));
	if (done > 20 && elapsed > 2.0 && done < total)
		printf(" · ETA ~%s",
		       human_duration((total - done) / (done / elapsed)));
	printf("\n");
}

static void print_total_progress(void)
{
	if (pdd.phase)
		print_dedupe_progress();
	else
		print_scan_progress();
}

static void prepare_screen_area(void)
{
	/*
	 * Prepare one empty line for each scan threads
	 * plus one line for the total.
	 * This is required to bypass the scrolling and let us
	 * save/restore the cursor position.
	 */
	for (unsigned int i = 0; i < options.io_threads + 3; i++)
		s_printf("\n");

	/* Go back to the first line */
	printf("\33[%iA", options.io_threads + 3);

	/*
	 * Save the cursor position.
	 * We will restore it every time we print progress.
	 */
	s_save_pos()
}

static void *print_progress(void)
{
	files_scanned = 0;
	bytes_scanned = 0;
	pscan.files_examined = 0;

	s_restore_pos();

	for (unsigned int i = 0; i < pscan.thread_count; i++) {
		print_thread_progress(pscan.threads[i]);
		files_scanned += pscan.threads[i]->total_scanned_files;
		bytes_scanned += pscan.threads[i]->total_scanned_bytes;
	}

	print_total_progress();

	return NULL;
}

static void *pscan_progress_thread(void * p)
{
	struct winsize w;
	do {
		/* Refresh the tty properties. Some ttys (e.g. bare ptys)
		 * report a zero width; treat that as "don't truncate". */
		if (tty) {
			ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
			w_col = w.ws_col ? w.ws_col : UINT_MAX;
		} else {
			w_col = UINT_MAX;
		}

		g_mutex_lock(&pscan.mutex);
		print_progress();
		g_mutex_unlock(&pscan.mutex);

		/* Do not waste too much cpu */
		usleep(1000 * (tty ? 100 : 1000));
	} while (pdd.phase ? pdd.running
			   : (!pscan.listing_completed
			      || files_scanned != pscan.total_files_count
			      || bytes_scanned != pscan.total_bytes_count));

	return NULL;
}

static void pscan_free_threads(void)
{
	for (unsigned int i = 0; i < pscan.thread_count; i++)
		free(pscan.threads[i]);
	free(pscan.threads);
	pscan.threads = NULL;
	pscan.thread_count = 0;
}

struct pscan_thread *pscan_register_thread(pid_t tid)
{
	struct pscan_thread *tprogress = calloc(1, sizeof(struct pscan_thread));
	tprogress->tid = tid;

	g_mutex_lock(&pscan.mutex);
	pscan.threads = realloc(pscan.threads, (pscan.thread_count + 1) *
						sizeof(struct pscan_thread *));
	pscan.threads[pscan.thread_count] = tprogress;
	pscan.thread_count++;
	g_mutex_unlock(&pscan.mutex);
	return tprogress;
}

void pscan_run(void)
{
	tty = isatty(STDOUT_FILENO);

	if (tty) {
		/* hide the cursor */
		printf("\33[?25l");

		prepare_screen_area();
	}

	/* Will abort on failure */
	printer = g_thread_new("progress_printer", pscan_progress_thread, NULL);
}

void pscan_join(void)
{
	g_thread_join(printer);

	/* Show the cursor again */
	printf("\33[?25h");

	/* Clear the screen from all thread-progress */
	s_restore_pos();
	s_clear();
	s_restore_pos();

	print_total_progress();

	pscan_free_threads();

	printer = NULL;
}

void pscan_reset_thread(struct pscan_thread **progress)
{
	if (!progress || !*progress)
		return;
	uint64_t scanned = (*progress)->file_scanned_bytes;
	uint64_t total = (*progress)->file_total_bytes;

	(*progress)->status = thread_idle;
	/*
	 * The file may have shrinked between the statx and
	 * the end of the scan.
	 * Does not matter much, we fake-fill the missing bytes
	 * so the global progress don't diverge much
	 */
	if (scanned < total)
		(*progress)->total_scanned_bytes += total - scanned;

	/*
	 * Also, the file may have grow.
	 */
	if (scanned > total)
		(*progress)->total_scanned_bytes -= scanned - total;

	(*progress)->total_scanned_files++;
	(*progress)->file_path[0] = '\0';
}

bool is_progress_printer_running(void)
{
	return printer ? true : false;
}

void pscan_printf(char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	g_mutex_lock(&pscan.mutex);
	if (tty) {
		s_restore_pos();
		s_clear();
		s_restore_pos();
	}

	vfprintf(stdout, fmt, args);

	if (tty) {
		s_save_pos();
		prepare_screen_area();
	}
	va_end(args);

	/*
	 * We reprint the progress immediately to reduce
	 * the time during which the screen is left empty:
	 * between the prepare_screen_area() and the progress_thread()'s
	 * next iteration.
	 */
	print_progress();
	g_mutex_unlock(&pscan.mutex);
}

/*
 * Claim a per-thread display slot for one dedupe group. Dedupe passes run on
 * short-lived exclusive thread pools (one per pass), so slots are claimed per
 * group and released via pscan_reset_thread() instead of being tied to the OS
 * thread - the number of live slots stays bounded by the pool size.
 */
struct pscan_thread *pdedupe_claim_slot(pid_t tid)
{
	struct pscan_thread *slot = NULL;

	g_mutex_lock(&pscan.mutex);
	for (unsigned int i = 0; i < pscan.thread_count; i++) {
		if (pscan.threads[i]->status == thread_idle) {
			slot = pscan.threads[i];
			slot->tid = tid;
			slot->status = thread_deduping;
			break;
		}
	}
	g_mutex_unlock(&pscan.mutex);

	if (!slot) {
		slot = pscan_register_thread(tid);
		slot->status = thread_deduping;
	}
	return slot;
}

void pdedupe_begin(uint64_t estimated_groups, unsigned int batches)
{
	pdd.done = 0;
	pdd.queued = 0;
	pdd.reclaimed = 0;
	pdd.kern = 0;
	pdd.estimate = estimated_groups;
	pdd.batches = batches;
	pdd.batch = batches ? 1 : 0;
	pdd.activity = "analyzing duplicates";
	pdd.start_us = g_get_monotonic_time();
	pdd.phase = true;

	/*
	 * The live area only makes sense on a tty; under -v the per-group
	 * notices would fight with the redraws, and -q wants silence. The
	 * counters above accumulate regardless, for the final summary.
	 */
	if (quiet || verbose || !isatty(STDOUT_FILENO))
		return;

	tty = true;
	printf("\33[?25l");	/* hide the cursor */
	prepare_screen_area();
	pdd.running = 1;
	printer = g_thread_new("progress_printer", pscan_progress_thread, NULL);
}

void pdedupe_end(void)
{
	if (pdd.running) {
		pdd.running = 0;
		g_thread_join(printer);
		printer = NULL;

		/* Show the cursor again and clear the status area; the caller
		 * prints the final summary. */
		printf("\33[?25h");
		s_restore_pos();
		s_clear();
		s_restore_pos();
		fflush(stdout);
	}
	pscan_free_threads();
	pdd.phase = false;
}

void pdedupe_set_batch(unsigned int cur)
{
	pdd.batch = cur;
}

void pdedupe_set_activity(const char *activity)
{
	pdd.activity = activity;
}

void pdedupe_add_queued(uint64_t ngroups)
{
	atomic_fetch_add(&pdd.queued, ngroups);
}

void pdedupe_group_done(uint64_t reclaimed_bytes, uint64_t kern_bytes)
{
	atomic_fetch_add(&pdd.done, 1);
	atomic_fetch_add(&pdd.reclaimed, reclaimed_bytes);
	atomic_fetch_add(&pdd.kern, kern_bytes);
}

void pdedupe_counters(uint64_t *groups, uint64_t *reclaimed, uint64_t *kern)
{
	*groups = pdd.done;
	*reclaimed = pdd.reclaimed;
	*kern = pdd.kern;
}

static void *psearch_progress_thread(void * p)
{
	static int last_pos = -1;

	do {
		int pos;
		int width = 40;

		pos = (float) search_processed / search_total * width;

		/* Only update our status every width% */
		if (pos > last_pos) {
			last_pos = pos;

			printf("\r[");
			for(int i = 0; i < width; i++) {
				if (i < pos)
					printf("#");
				else if (i == pos)
					printf("%%");
				else
					printf(" ");
			}
			printf("]");
		}

		/* Do not waste too much cpu */
		usleep(100);
	} while (search_total != search_processed);
	printf("\n");
	return NULL;
}

void psearch_run(uint64_t num_filerecs)
{
	search_processed = 0;
	search_total = num_filerecs;

	/*
	 * During the dedupe phase the search is rendered as a live count on
	 * the shared status area; the standalone bar is only for runs without
	 * that area (e.g. print-only mode).
	 */
	if (pdd.phase) {
		pdedupe_set_activity("searching block-level matches");
		return;
	}
	printer = g_thread_new("progress_printer", psearch_progress_thread, NULL);
}

void psearch_join(void)
{
	if (pdd.phase) {
		search_total = 0;
		search_processed = 0;
		return;
	}
	g_thread_join(printer);
	printer = NULL;
}

void psearch_update_processed_count(unsigned int processed)
{
	atomic_fetch_add(&search_processed, processed);
}
