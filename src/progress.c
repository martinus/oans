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
#include <inttypes.h>

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

/*
 * When set (--progress=json), the progress thread streams newline-delimited
 * JSON to stderr once a second instead of drawing the ANSI block, and all the
 * cursor/screen handling is skipped. Works whether or not stdout is a tty, so
 * scheduled / non-interactive runs can be monitored. stdout is untouched.
 */
static bool progress_json;

void progress_set_json(bool on)
{
	progress_json = on;
}

/*
 * Rows the live progress block occupied in the last render. The redraw returns
 * to the top of the block by moving the cursor up this many rows (a *relative*
 * move, which stays correct when the terminal scrolls) rather than restoring an
 * absolute saved position (which a scroll invalidates, leaving stale copies of
 * the block piling up). 0 means "no block drawn yet - draw fresh here".
 */
static unsigned int drawn_lines;

/*
 * The unified live display walks a run through four monotonic stages. The stage
 * line shows all four at once, each with a spinner (running), a tick (finished)
 * or a dim dot (pending); the progress bar and detail line below it describe
 * whichever stage is the current live edge. See docs/progress-ui-spec.md.
 */
enum stage { STAGE_SCAN, STAGE_HASH, STAGE_DEDUPE, STAGE_DONE, STAGE_COUNT };
enum stage_state { ST_PENDING, ST_RUNNING, ST_DONE };

static enum stage_state stages[STAGE_COUNT];

static void stage_set(enum stage s, enum stage_state st)
{
	stages[s] = st;
}

/* Sums of the per-thread stats */
static uint64_t files_scanned, bytes_scanned;

/*
 * ETA by weighted progress. Hashing a file costs roughly k*bytes + d: a
 * per-byte rate plus a fixed per-file overhead (open, fiemap, the DB write).
 * Rather than fit k and d - which is ill-conditioned while only big files have
 * been hashed, and blows the ETA up to hours on a cold scan when the per-file
 * overhead spikes under I/O contention - we fix the *ratio* d/k as a constant
 * file weight (in byte-equivalents) and let the elapsed time supply the scale.
 *
 * Each file counts as `weight` bytes of work, so total work = bytes +
 * weight*files is known from the listing up front (no warm-up, no discovery of
 * the small-file tail mid-scan). The remaining fraction is extrapolated by the
 * time already spent, which measures the true speed empirically - parallelism,
 * cache state and device all fold into elapsed/done. A wrong weight only
 * reweights files vs bytes and is self-limiting (too large and it degrades to a
 * plain file-rate ETA; it cannot run away), so a fixed value per storage class
 * is robust where fitting d was not.
 */
#define ETA_FILE_WEIGHT_SSD	(32u << 10)	/* 32 KiB: measured d/k ~30 KiB on flash */
#define ETA_FILE_WEIGHT_HDD	(256u << 10)	/* 256 KiB: measured d/k ~268 KiB on a
						 * 4-HDD btrfs RAID (1.7M files, cold) */
static uint64_t eta_file_weight = ETA_FILE_WEIGHT_SSD;

void pscan_set_storage_rotational(bool rotational)
{
	eta_file_weight = rotational ? ETA_FILE_WEIGHT_HDD : ETA_FILE_WEIGHT_SSD;
}

uint64_t pscan_eta_file_weight(void)
{
	return eta_file_weight;
}

/*
 * Seconds left, or -1 if nothing measurable yet. weight is the per-file work in
 * byte-equivalents. Pure, unit-tested (see tests.c).
 */
static double scan_eta_seconds(uint64_t done_bytes, uint64_t done_files,
			       uint64_t total_bytes, uint64_t total_files,
			       uint64_t weight, double elapsed)
{
	double done = (double)done_bytes + (double)weight * done_files;
	double total = (double)total_bytes + (double)weight * total_files;

	if (done <= 0.0)
		return -1.0;
	if (total <= done)
		return 0.0;
	return elapsed * (total - done) / done;
}

/*
 * Dedupe ETA: linear extrapolation from the rate so far, once enough groups are
 * done for it to be stable. -1 until measurable. Shared by the live bar and the
 * JSON stream so they never diverge.
 */
static double dedupe_eta_seconds(uint64_t done, uint64_t total, double elapsed)
{
	if (done > 20 && elapsed > 2.0 && done < total)
		return (double)(total - done) / (done / elapsed);
	return -1.0;
}

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
	/* reclaimed: honest disk freed (kernel-deduped bytes). net_shared: fiemap
	 * "net change in shared extents", a diagnostic for the machine-readable
	 * line only (it counts the surviving copy as shared too, so ~2x for pairs). */
	_Atomic uint64_t	reclaimed, net_shared;
} pdd;

#define s_printf(args...) do { if (tty) printf("\33[K"); printf(args); } while (0)

/* Move the cursor to the top-left of the block drawn in the previous render. */
static void progress_home(void)
{
	if (tty && drawn_lines)
		printf("\33[%uA\r", drawn_lines);
}

/* Erase from the cursor to the end of the screen (removes any rows a taller
 * previous render left below the current one). */
static void progress_wipe(void)
{
	if (tty)
		printf("\33[J");
}

#define percent(val1, val2) \
	((val2) ? (double) (val1) / (double) (val2) * 100 : 0.0)

void pscan_finish_listing(void)
{
	pscan.listing_completed = true;
	stage_set(STAGE_SCAN, ST_DONE);
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

/* Files that needed (re)hashing this run - the work actually done. Distinct
 * from files_examined, which counts every file the walk visited (including
 * those already up to date). */
uint64_t pscan_files_scanned(void)
{
	return pscan.total_files_count;
}

#define BUF_LEN 10*1024
/*
 * Fit `path` into at most `cols` columns by replacing its middle with a
 * single ellipsis, so both the leading directories and the filename stay
 * readable. The filename end matters more, so it gets the bigger share.
 */
static void ellipsize_path(const char *path, char *out, size_t out_len,
			   int cols)
{
	int len = strlen(path);
	int head, tail;

	if (cols >= len) {
		snprintf(out, out_len, "%s", path);
		return;
	}
	if (cols < 8)
		cols = 8;

	head = (cols - 1) * 2 / 5;
	tail = cols - 1 - head;
	snprintf(out, out_len, "%.*s…%s", head, path, path + len - tail);
}

static const char *const stage_name[STAGE_COUNT] = {
	[STAGE_SCAN]	= "scanning",
	[STAGE_HASH]	= "hashing",
	[STAGE_DEDUPE]	= "dedupe",
	[STAGE_DONE]	= "done",
};

/* Accent color per stage (also colors that stage's bar and prefix). */
static const char *stage_color(enum stage s)
{
	switch (s) {
	case STAGE_SCAN:	return col_blue;
	case STAGE_HASH:	return col_cyan;
	case STAGE_DEDUPE:	return col_green;
	default:		return col_green;
	}
}

/* Braille spinner cycle; one frame advances per render (see the timer loop). */
static const char *const spinner_frames[] = {
	"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
};
static unsigned int spin_frame;

static const char *spinner_glyph(void)
{
	return tty ? spinner_frames[spin_frame % ARRAY_SIZE(spinner_frames)]
		   : "*";
}

/* Progress-bar cells: full, an 8-level sub-cell ramp, and the empty dot. */
static const char *const BAR_SUB[] = {
	" ", "⡀", "⡄", "⡆", "⡇", "⣇", "⣧", "⣷", "⣿",
};
#define BAR_FULL	"⣿"
#define BAR_EMPTY	"·"
#define BAR_WIDTH	40
#define STAGE_PREFIX_W	8	/* widest stage name ("scanning") */
#define STATUS_COL_W	9	/* widest worker status ("wait lock") */
/* Worker-line left column: number(3) + gap(2) + status + gap(2) before the path. */
#define WORKER_LEFT_W	(3 + 2 + STATUS_COL_W + 2)

/* The dim " · " separator between fields on the bar and detail lines. */
static void detail_sep(void)
{
	printf(" %s·%s ", col_dim, col_reset);
}

/* Worker status word (no colon) and its color. thread_scanning is "hashing". */
static const char *status_word(enum pscan_thread_status s)
{
	switch (s) {
	case thread_idle:		return "idle";
	case thread_mapping:		return "mapping";
	case thread_scanning:		return "hashing";
	case thread_waiting_lock:	return "wait lock";
	case thread_committing:		return "commit";
	case thread_deduping:		return "deduping";
	}
	return "";
}

static const char *status_color(enum pscan_thread_status s)
{
	switch (s) {
	case thread_idle:		return col_dim;
	case thread_mapping:		return col_blue;
	case thread_scanning:		return col_cyan;
	case thread_waiting_lock:	return col_yellow;
	case thread_committing:		return col_magenta;
	case thread_deduping:		return col_green;
	}
	return col_reset;
}

static void print_thread_progress(struct pscan_thread *t, unsigned int slot)
{
	char buf[BUF_LEN];
	char path[PATH_MAX + 4], clean[PATH_MAX + 1];
	char m_plain[160], m_col[512];
	const char *word = status_word(t->status);
	const char *wcol = status_color(t->status);
	int avail, termw;

	/* Idle slots are just the dim number and word - nothing on the right. */
	if (t->status == thread_idle) {
		s_printf("%s%3u%s  %s%s%s\n",
			 col_dim, slot, col_reset, wcol, word, col_reset);
		return;
	}

	/*
	 * The metrics are built twice: a plain copy (to budget the path width by
	 * visible columns) and a colored copy (what is actually printed). The
	 * done size is bold, the "/" and total are dim, the percent takes the
	 * status color - so the numbers stand out from the path.
	 */
	switch (t->status) {
	case thread_scanning:
	case thread_deduping: {
		char ds[32], ts[32];
		double p = percent(t->file_scanned_bytes, t->file_total_bytes);

		human_size_snprintf(t->file_scanned_bytes, ds, sizeof(ds));
		human_size_snprintf(t->file_total_bytes, ts, sizeof(ts));
		snprintf(m_plain, sizeof(m_plain), "%s/%s (%05.2f%%)", ds, ts, p);
		snprintf(m_col, sizeof(m_col),
			 "%s%s%s%s/%s%s %s(%05.2f%%)%s",
			 col_bold, ds, col_reset, col_dim, ts, col_reset,
			 wcol, p, col_reset);
		break;
	}
	default: {	/* mapping / wait lock / commit: size only, no bytes yet */
		char ss[32];

		human_size_snprintf(t->file_total_bytes, ss, sizeof(ss));
		snprintf(m_plain, sizeof(m_plain), "(size: %s)", ss);
		snprintf(m_col, sizeof(m_col), "%s(size: %s)%s",
			 col_dim, ss, col_reset);
		break;
	}
	}

	/*
	 * Give the path whatever width remains after the fixed prefix (number +
	 * status column), a two-space gap, and the metrics, then shorten its
	 * middle. When the terminal width is unknown (some SSH ptys report 0
	 * columns), fall back to 80 so a long path can't wrap onto a second row
	 * and desync the fixed-height area. Color codes are zero-width, so the
	 * budget is computed from the plain metrics only.
	 */
	termw = (int)(w_col == UINT_MAX ? 80 : w_col);
	avail = termw - WORKER_LEFT_W - (int)strlen(m_plain) - 2;	/* -2: gap before metrics */
	/* Never emit raw control bytes from a filename to the terminal (#353). */
	sanitize_ctrl(t->file_path, clean, sizeof(clean));
	ellipsize_path(clean, path, sizeof(path), avail);

	snprintf(buf, BUF_LEN, "%s%3u%s  %s%-*s%s  %s  %s",
		 col_dim, slot, col_reset,
		 wcol, STATUS_COL_W, word, col_reset, path, m_col);
	s_printf("%s\n", buf);
}

/* The four-word stage line: glyph + word per stage, colored by tri-state. */
static unsigned int print_stage_line(void)
{
	if (tty)
		fputs("\033[K", stdout);

	for (enum stage s = 0; s < STAGE_COUNT; s++) {
		const char *acc = stage_color(s);

		if (s)
			fputs("   ", stdout);

		switch (stages[s]) {
		case ST_DONE:
			printf("%s%s%s %s%s%s", col_green, tty ? "✔" : "x",
			       col_reset, col_green, stage_name[s], col_reset);
			break;
		case ST_RUNNING:
			printf("%s%s%s %s%s%s%s", acc, spinner_glyph(), col_reset,
			       col_bold, acc, stage_name[s], col_reset);
			break;
		default:	/* ST_PENDING */
			printf("%s%s %s%s", col_dim, tty ? "·" : "-",
			       stage_name[s], col_reset);
			break;
		}
	}
	putchar('\n');
	return 1;
}

/*
 * A BAR_WIDTH-cell braille bar in the stage accent color. `indet` draws a
 * bouncing lit window (used while the hashing total is still unknown); else the
 * bar fills to `frac` with an 8-level sub-cell for the fractional part.
 */
static void render_bar(double frac, bool indet, const char *acc)
{
	putchar('[');

	if (!tty) {	/* plain ASCII for logs / non-tty */
		int f = indet ? 0 : (int)(frac * BAR_WIDTH + 0.5);

		for (int i = 0; i < BAR_WIDTH; i++)
			putchar(i < f ? '#' : '-');
		putchar(']');
		return;
	}

	if (indet) {
		int win = 8, span = BAR_WIDTH - win;
		int q = span ? (int)(spin_frame % (2u * (unsigned)span)) : 0;
		int pos = q <= span ? q : 2 * span - q;

		for (int i = 0; i < BAR_WIDTH; i++) {
			bool lit = i >= pos && i < pos + win;

			if (lit)
				printf("%s%s%s", acc, BAR_FULL, col_reset);
			else
				printf("%s%s%s", col_dim, BAR_EMPTY, col_reset);
		}
	} else {
		int eighths, full, rem, printed = 0;

		if (frac < 0.0)
			frac = 0.0;
		if (frac > 1.0)
			frac = 1.0;
		eighths = (int)(frac * BAR_WIDTH * 8 + 0.5);
		full = eighths / 8;
		rem = eighths % 8;

		printf("%s", acc);
		for (; printed < full && printed < BAR_WIDTH; printed++)
			fputs(BAR_FULL, stdout);
		if (rem && printed < BAR_WIDTH) {
			fputs(BAR_SUB[rem], stdout);
			printed++;
		}
		printf("%s%s", col_reset, col_dim);
		for (; printed < BAR_WIDTH; printed++)
			fputs(BAR_EMPTY, stdout);
		printf("%s", col_reset);
	}
	putchar(']');
}

/* The current live-edge stage: dedupe once that phase is running, else hashing. */
static enum stage current_stage(void)
{
	return pdd.phase ? STAGE_DEDUPE : STAGE_HASH;
}

/* Stage-prefixed progress bar + percent + ETA for the current stage. */
static unsigned int print_bar_line(void)
{
	enum stage cs = current_stage();
	const char *acc = stage_color(cs);
	bool indet = false;
	double frac = 0.0, eta = -1.0;
	unsigned int pct = 0;

	if (pdd.phase) {
		uint64_t done = pdd.done, queued = pdd.queued;
		uint64_t total = pdd.estimate > queued ? pdd.estimate : queued;
		double elapsed = (g_get_monotonic_time() - pdd.start_us) / 1e6;

		/* Fuzzy total: cap at 99% while the phase is still running. */
		if (total) {
			frac = (double)done / total;
			pct = (unsigned int)(100.0 * done / total);
			if (pct > 99)
				pct = 99;
		} else {
			/* No group count yet (still analyzing): bounce the bar. */
			indet = true;
		}
		eta = dedupe_eta_seconds(done, total, elapsed);
	} else if (!pscan.listing_completed) {
		/* Hashing total not known until the listing finishes. */
		indet = true;
	} else {
		uint64_t tf = pscan.total_files_count, tb = pscan.total_bytes_count;
		double elapsed = elapsed_seconds();
		double done = (double)bytes_scanned +
			      (double)eta_file_weight * files_scanned;
		double total = (double)tb + (double)eta_file_weight * tf;

		frac = total > 0.0 ? done / total : 1.0;
		pct = (unsigned int)(frac * 100.0);
		eta = scan_eta_seconds(bytes_scanned, files_scanned, tb, tf,
				       eta_file_weight, elapsed);
	}

	s_printf("%s%-*s%s  ", acc, STAGE_PREFIX_W, stage_name[cs], col_reset);
	render_bar(frac, indet, acc);
	if (!indet) {
		printf("  %s%u%%%s", col_bold, pct, col_reset);
		if (eta > 0.0) {
			detail_sep();
			printf("ETA ~%s", human_duration(eta));
		}
	}
	putchar('\n');
	return 1;
}

/*
 * One line of concrete numbers for the current stage. Indented to line up under
 * the bar (the stage-name prefix + its two spaces) and with no leading word -
 * the stage line above already names the phase.
 */
static unsigned int print_detail_line(void)
{
	s_printf("%*s", STAGE_PREFIX_W + 2, "");

	if (pdd.phase) {
		uint64_t done = pdd.done, queued = pdd.queued;
		uint64_t total = pdd.estimate > queued ? pdd.estimate : queued;
		uint64_t sd = search_processed, st = search_total;
		bool pool_idle = true;

		/*
		 * The dedupe pool drains between batches while the main thread
		 * loads / groups the next batch, so every worker slot sits idle for
		 * a beat. Detect that and surface the current activity, so those
		 * pauses read as work-in-progress rather than a hang.
		 */
		for (unsigned int i = 0; i < pscan.thread_count; i++)
			if (pscan.threads[i]->status != thread_idle) {
				pool_idle = false;
				break;
			}

		/*
		 * Before the first group is deduped there is no numeric progress
		 * yet - show what the phase is doing (analyzing duplicates, loading
		 * identical files, ...) here under the bar instead of "0 / ~0".
		 */
		if (done == 0 && pdd.activity) {
			printf("%s", pdd.activity);
			if (st) {
				detail_sep();
				printf("%s/%s files", group_u64(sd), group_u64(st));
			}
			putchar('\n');
			return 1;
		}

		printf("%s%s%s / ~%s groups",
		       col_bold, group_u64(done), col_reset, group_u64(total));
		if (pdd.batches > 1) {
			detail_sep();
			printf("batch %u/%u", pdd.batch, pdd.batches);
		}
		if (st) {
			detail_sep();
			printf("searching extents %s/%s", group_u64(sd), group_u64(st));
		} else if (pool_idle && pdd.activity) {
			detail_sep();
			printf("%s", pdd.activity);
		}
		detail_sep();
		printf("reclaimed %s%s%s\n", col_green,
		       human_size(pdd.reclaimed), col_reset);
		return 1;
	}

	if (!pscan.listing_completed) {
		/*
		 * files_examined is the cumulative count of files the walk has
		 * visited (reset once at pscan_run); total_files_count is how many
		 * of them need (re)hashing. Both climb monotonically during listing.
		 */
		printf("%s%s%s files", col_bold,
		       group_u64(pscan.files_examined), col_reset);
		detail_sep();
		printf("%s%s%s need hashing\n", col_bold,
		       group_u64(pscan.total_files_count), col_reset);
		return 1;
	}

	{
		uint64_t tf = pscan.total_files_count, tb = pscan.total_bytes_count;
		double elapsed = elapsed_seconds();

		printf("%s%s%s / %s files", col_bold,
		       group_u64(files_scanned), col_reset, group_u64(tf));
		detail_sep();
		printf("%s / %s", human_size(bytes_scanned), human_size(tb));
		if (bytes_scanned && elapsed > 1.0) {
			detail_sep();
			printf("%s/s", human_size((uint64_t)(bytes_scanned / elapsed)));
		}
		putchar('\n');
		return 1;
	}
}

/* Render the whole live totals block (stage line, bar, detail). */
static unsigned int print_total_progress(void)
{
	unsigned int lines = 0;

	lines += print_stage_line();
	lines += print_bar_line();
	lines += print_detail_line();
	return lines;
}

static void prepare_screen_area(void)
{
	/*
	 * The relative-redraw model self-manages: the next print_progress()
	 * draws the block at the current cursor, and later ones move up
	 * drawn_lines to redraw in place. Just declare "no block drawn yet".
	 */
	drawn_lines = 0;
}

/* Refresh the per-run scanned totals from the live slots. Caller holds mutex. */
static void sum_scanned(void)
{
	files_scanned = bytes_scanned = 0;
	for (unsigned int i = 0; i < pscan.thread_count; i++) {
		files_scanned += pscan.threads[i]->total_scanned_files;
		bytes_scanned += pscan.threads[i]->total_scanned_bytes;
	}
}

static void *print_progress(void)
{
	unsigned int lines = 0;

	sum_scanned();

	progress_home();	/* back to the top of the block we drew last time */

	/* Worker lines on top, numbered 1..N (the slot index, not the pid). */
	for (unsigned int i = 0; i < pscan.thread_count; i++) {
		print_thread_progress(pscan.threads[i], i + 1);
		lines++;
	}

	/* One blank spacer line between the workers and the stage indicator. */
	s_printf("\n");
	lines++;

	lines += print_total_progress();

	progress_wipe();	/* drop any rows a taller previous render left */
	drawn_lines = lines;

	return NULL;
}

/* The live edge for the JSON stream: scanning and hashing overlap, so the
 * hashing tick begins once the listing is complete; dedupe once it starts. */
enum jphase { JP_SCAN, JP_HASH, JP_DEDUPE };

static enum jphase json_phase(void)
{
	return pdd.phase ? JP_DEDUPE :
	       (pscan.listing_completed ? JP_HASH : JP_SCAN);
}

/*
 * One newline-delimited JSON progress object on stderr for the given phase.
 * Numbers are raw (bytes, file/group counts); *_sec fields are seconds. Fields
 * that are not yet measurable (ETA, rate) are simply omitted. Consumers read a
 * line at a time and can ignore any line that is not valid JSON (e.g. an
 * interleaved error). files_scanned/bytes_scanned are the per-run sums the
 * caller refreshed this tick. See docs/man/oans.md.
 */
static void emit_json_progress(enum jphase phase)
{
	double elapsed = elapsed_seconds();

	switch (phase) {
	case JP_DEDUPE: {
		uint64_t done = pdd.done, queued = pdd.queued;
		uint64_t total = pdd.estimate > queued ? pdd.estimate : queued;
		uint64_t st = search_total, sd = search_processed;
		double de = (g_get_monotonic_time() - pdd.start_us) / 1e6;
		double eta = dedupe_eta_seconds(done, total, de);

		fprintf(stderr, "{\"phase\":\"dedupe\",\"elapsed_sec\":%.2f,"
			"\"groups\":%" PRIu64 ",\"groups_total\":%" PRIu64 ","
			"\"reclaimed_bytes\":%" PRIu64, elapsed, done, total,
			(uint64_t)pdd.reclaimed);
		if (pdd.activity)
			fprintf(stderr, ",\"activity\":\"%s\"", pdd.activity);
		if (st)
			fprintf(stderr, ",\"search_files\":%" PRIu64 ","
				"\"search_files_total\":%" PRIu64, sd, st);
		if (eta > 0.0)
			fprintf(stderr, ",\"eta_sec\":%.1f", eta);
		break;
	}
	case JP_SCAN:
		fprintf(stderr, "{\"phase\":\"scanning\",\"elapsed_sec\":%.2f,"
			"\"files_examined\":%" PRIu64 ",\"files_to_hash\":%" PRIu64,
			elapsed, pscan.files_examined, pscan.total_files_count);
		break;
	case JP_HASH: {
		uint64_t tf = pscan.total_files_count, tb = pscan.total_bytes_count;

		fprintf(stderr, "{\"phase\":\"hashing\",\"elapsed_sec\":%.2f,"
			"\"files\":%" PRIu64 ",\"files_total\":%" PRIu64 ","
			"\"bytes\":%" PRIu64 ",\"bytes_total\":%" PRIu64,
			elapsed, files_scanned, tf, bytes_scanned, tb);
		if (bytes_scanned && elapsed > 1.0) {
			double eta = scan_eta_seconds(bytes_scanned, files_scanned,
						      tb, tf, eta_file_weight, elapsed);

			fprintf(stderr, ",\"bytes_per_sec\":%.0f",
				bytes_scanned / elapsed);
			if (eta > 0.0)
				fprintf(stderr, ",\"eta_sec\":%.1f", eta);
		}
		break;
	}
	}
	fprintf(stderr, "}\n");
	fflush(stderr);
}

/* Emitted once at the end of a run when --progress=json is set. */
void pscan_json_done(uint64_t files_scanned, uint64_t groups, uint64_t reclaimed)
{
	if (!progress_json)
		return;
	fprintf(stderr, "{\"event\":\"done\",\"elapsed_sec\":%.2f,"
		"\"files_scanned\":%" PRIu64 ",\"groups_deduped\":%" PRIu64 ","
		"\"reclaimed_bytes\":%" PRIu64 "}\n",
		elapsed_seconds(), files_scanned, groups, reclaimed);
	fflush(stderr);
}

static void *pscan_progress_thread(void * p)
{
	struct winsize w;
	gint64 json_last = 0;	/* monotonic us of the last JSON emit */
	enum jphase json_last_phase = -1;

	do {
		if (progress_json) {
			gint64 now = g_get_monotonic_time();
			enum jphase phase = json_phase();

			/* Refresh the per-run sums every tick so the loop's exit test
			 * stays responsive (~100ms). Emit ~1/s, but always right away
			 * on a phase change so every phase is reported even on a fast
			 * run. */
			g_mutex_lock(&pscan.mutex);
			sum_scanned();
			g_mutex_unlock(&pscan.mutex);

			if (phase != json_last_phase || now - json_last >= 1000000) {
				emit_json_progress(phase);
				json_last = now;
				json_last_phase = phase;
			}
			usleep(100 * 1000);
			continue;
		}

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

		spin_frame++;	/* advance every spinner + the indeterminate bar */

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

	/* Scanning and hashing run together from the start; dedupe follows. */
	stage_set(STAGE_SCAN, ST_RUNNING);
	stage_set(STAGE_HASH, ST_RUNNING);
	stage_set(STAGE_DEDUPE, ST_PENDING);
	stage_set(STAGE_DONE, ST_PENDING);
	spin_frame = 0;
	pscan.files_examined = 0;	/* cumulative walk count, climbs to the total */

	if (tty && !progress_json) {
		/* hide the cursor */
		printf("\33[?25l");

		prepare_screen_area();
	}

	/* Will abort on failure */
	printer = g_thread_new("progress_printer", pscan_progress_thread, NULL);
}

void pscan_join(bool continues)
{
	g_thread_join(printer);
	printer = NULL;

	/* The listing is done (STAGE_SCAN) and the csum pool has drained. */
	stage_set(STAGE_HASH, ST_DONE);

	/* JSON mode draws no block: nothing to leave or wipe, just release the
	 * scan slots (the dedupe phase re-claims its own). */
	if (progress_json) {
		pscan_free_threads();
		return;
	}

	if (continues) {
		/*
		 * A live dedupe phase will keep drawing this same block, so leave
		 * it in place - workers and all - and just refresh it once so
		 * hashing shows ticked. Nothing is wiped or stranded above the
		 * dedupe view, and the worker list never blinks away. Threads,
		 * drawn_lines and the hidden cursor are kept for the dedupe phase
		 * (it reuses the now-idle slots and redraws over this block).
		 */
		g_mutex_lock(&pscan.mutex);
		print_progress();
		g_mutex_unlock(&pscan.mutex);
		return;
	}

	/*
	 * No live dedupe follows (print-only / non-tty / -v): wipe the live area
	 * clean so the report or the next output starts on a fresh line. No
	 * summary is left behind.
	 */
	if (tty)
		printf("\33[?25h");	/* show the cursor again */
	progress_home();
	progress_wipe();
	drawn_lines = 0;
	pscan_free_threads();
}

void pscan_reset_thread(struct pscan_thread **progress)
{
	/*
	 * The churning dedupe pool re-claims a slot per work item, so its reset
	 * is just "finish this file's accounting, then park the slot idle" - the
	 * two persistent-slot primitives composed, so the byte reconciliation has
	 * a single source of truth.
	 */
	pscan_finish_file(progress);
	if (progress && *progress)
		pscan_slot_idle(*progress);
}

/*
 * Roll a persistently-held slot from one file to the next: do the per-file
 * accounting pscan_reset_thread() does (reconcile scanned-vs-total bytes, bump
 * the file count) but leave the slot claimed and non-idle, so a worker that
 * hands off directly to its next file never flashes "idle" in the microsecond
 * gap between them. The status/path stay showing the just-finished file until
 * the next one overwrites them. _cleanup_ compatible.
 */
void pscan_finish_file(struct pscan_thread **progress)
{
	uint64_t scanned, total;

	if (!progress || !*progress)
		return;
	scanned = (*progress)->file_scanned_bytes;
	total = (*progress)->file_total_bytes;

	/*
	 * The file may have shrunk between the statx and the end of the scan;
	 * fake-fill the missing bytes so the global progress doesn't diverge.
	 * It may also have grown, so trim the overshoot.
	 */
	if (scanned < total)
		(*progress)->total_scanned_bytes += total - scanned;
	if (scanned > total)
		(*progress)->total_scanned_bytes -= scanned - total;

	(*progress)->total_scanned_files++;
}

/*
 * Park a persistently-held slot as idle once its worker has no more work (drain).
 * No per-file accounting - pscan_finish_file() already ran for the last file.
 */
void pscan_slot_idle(struct pscan_thread *slot)
{
	if (!slot)
		return;
	slot->status = thread_idle;
	slot->file_path[0] = '\0';
}

bool is_progress_printer_running(void)
{
	return printer ? true : false;
}

void pscan_printf(char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	/* JSON mode draws no block (and the progress stream is on stderr), so a
	 * routed message is just a plain stdout print. */
	if (progress_json) {
		vfprintf(stdout, fmt, args);
		va_end(args);
		return;
	}

	g_mutex_lock(&pscan.mutex);

	/* Erase the live block, print the message where it sat (it becomes
	 * scrollback), then redraw the block below it. */
	progress_home();
	progress_wipe();
	drawn_lines = 0;

	vfprintf(stdout, fmt, args);
	va_end(args);

	print_progress();
	g_mutex_unlock(&pscan.mutex);
}

/*
 * Claim a per-thread display slot for one unit of work (a file scan, a dedupe
 * group). Worker pools churn OS threads (glib reaps idle pool threads and
 * spawns fresh ones), so slots are claimed per work item and released via
 * pscan_reset_thread() instead of being tied to the OS thread - dead threads
 * can't strand "idle" lines and the number of live slots stays bounded by the
 * pool size instead of growing with thread turnover.
 */
struct pscan_thread *pscan_claim_slot(pid_t tid,
				      enum pscan_thread_status status)
{
	struct pscan_thread *slot = NULL;

	g_mutex_lock(&pscan.mutex);
	for (unsigned int i = 0; i < pscan.thread_count; i++) {
		if (pscan.threads[i]->status == thread_idle) {
			slot = pscan.threads[i];
			slot->tid = tid;
			slot->status = status;
			break;
		}
	}
	g_mutex_unlock(&pscan.mutex);

	if (!slot) {
		slot = pscan_register_thread(tid);
		slot->status = status;
	}
	return slot;
}

void pdedupe_begin(unsigned int batches)
{
	pdd.done = 0;
	pdd.queued = 0;
	pdd.reclaimed = 0;
	pdd.net_shared = 0;
	pdd.estimate = 0;		/* set later via pdedupe_set_estimate() */
	pdd.batches = batches;
	pdd.batch = batches ? 1 : 0;
	pdd.activity = "analyzing duplicates";
	pdd.start_us = g_get_monotonic_time();
	pdd.phase = true;

	/* Reaching dedupe means scanning and hashing are behind us. */
	stage_set(STAGE_SCAN, ST_DONE);
	stage_set(STAGE_HASH, ST_DONE);
	stage_set(STAGE_DEDUPE, ST_RUNNING);
	stage_set(STAGE_DONE, ST_PENDING);
	spin_frame = 0;

	/*
	 * JSON progress runs regardless of tty/-q/-v (it draws no block). The
	 * interactive block only makes sense on a tty; under -v the per-group
	 * notices would fight the redraws, and -q wants silence. The counters
	 * above accumulate regardless, for the final summary.
	 */
	if (!progress_json && (quiet || verbose || !isatty(STDOUT_FILENO)))
		return;

	if (!progress_json) {
		tty = true;
		printf("\33[?25l");	/* hide the cursor (a no-op if scan already did) */
		/*
		 * Do NOT reset drawn_lines here: the scan left its block in place
		 * (see pscan_join with continues=true), so the first dedupe render
		 * redraws over that same block, keeping the worker list continuous.
		 * When there was no scan block (drawn_lines already 0) it draws fresh.
		 */
	}
	pdd.running = 1;
	printer = g_thread_new("progress_printer", pscan_progress_thread, NULL);
}

void pdedupe_end(void)
{
	/* Whole run is finished: tick dedupe and done. */
	stage_set(STAGE_DEDUPE, ST_DONE);
	stage_set(STAGE_DONE, ST_DONE);

	if (pdd.running) {
		pdd.running = 0;
		g_thread_join(printer);
		printer = NULL;

		/* Show the cursor again, wipe the live block, and leave the
		 * fully-ticked stage line in place; the caller prints the final
		 * summary below it. (JSON mode drew no block.) */
		if (!progress_json) {
			printf("\33[?25h");
			progress_home();
			progress_wipe();
			drawn_lines = 0;
			print_stage_line();
			fflush(stdout);
		}
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

void pdedupe_set_estimate(uint64_t estimated_groups)
{
	pdd.estimate = estimated_groups;
}

void pdedupe_add_queued(uint64_t ngroups)
{
	atomic_fetch_add(&pdd.queued, ngroups);
}

void pdedupe_group_done(uint64_t reclaimed_bytes, uint64_t net_shared_bytes)
{
	atomic_fetch_add(&pdd.done, 1);
	atomic_fetch_add(&pdd.reclaimed, reclaimed_bytes);
	atomic_fetch_add(&pdd.net_shared, net_shared_bytes);
}

void pdedupe_counters(uint64_t *groups, uint64_t *reclaimed, uint64_t *net_shared)
{
	*groups = pdd.done;
	*reclaimed = pdd.reclaimed;
	if (net_shared)
		*net_shared = pdd.net_shared;
}

static void *psearch_progress_thread(void * p)
{
	static int last_pos = -1;

	do {
		int pos;
		int width = 40;

		/* Guard the empty-search case so pos is 0, not NaN (#348). */
		pos = search_total ?
			(float) search_processed / search_total * width : width;

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
		usleep(100 * 1000);
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
