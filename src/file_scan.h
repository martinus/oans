#ifndef	__FILE_SCAN_H__
#define	__FILE_SCAN_H__

#include <sys/types.h>
#include <stdbool.h>
#include <uuid/uuid.h>

#include "dbfile.h"

#include "csum.h"

#define MIN_BLOCKSIZE   (4U*1024)
/* max blocksize is somewhat arbitrary. */
#define MAX_BLOCKSIZE   (1024U*1024)
#define DEFAULT_BLOCKSIZE       (128U*1024)

/*
 * Returns nonzero on fatal errors only
 */
int scan_file(char *name, struct dbhandle *db);

/*
 * Parallel directory walk. Bracket the scan_file() seeding calls with
 * filescan_walk_begin()/filescan_walk_run(): begin sets up the walk queues,
 * scan_file() seeds each root, and run() spawns the walker pool and consumes
 * every file found on the calling thread.
 */
void filescan_walk_begin(void);
int filescan_walk_run(struct dbhandle *db);

/*
 * Remove hashfile rows for files deleted from disk since the last scan. Uses
 * the walk's seen-set (built during filescan_walk_run) to skip re-stat()ing
 * files it already confirmed, so call this after scan_files() returns. Returns
 * the number of rows pruned, or -1 on error.
 */
int64_t filescan_prune_deleted(struct dbhandle *db);

void fs_get_locked_uuid(uuid_t *uuid);

/*
 * True after filescan_walk_run() when no root could be seeded because none
 * could be locked onto a supported filesystem (e.g. XFS whose UUID could not
 * be read without root on an old kernel). Lets the caller fail loudly instead
 * of reporting a silent, successful no-op.
 */
bool filescan_seed_failed(void);

/*
 * Scan-phase skip accounting (#145).
 *
 * Every "I skipped this" path in the walk used to write a line to stderr and be
 * forgotten: nothing counted them, so an unattended run that lost a whole
 * subtree to a permissions change looked identical to a healthy one in --json,
 * --history and the summary.
 *
 * Bucketed by cause, chosen so each maps to a distinct operator action. The
 * first four are problems; the last three are the user's own configuration
 * doing its job and must be reported apart from them, or "812 skipped" reads
 * as alarming when 812 of them are --exclude hits.
 */
enum scan_skip_bucket {
	SCAN_SKIP_PERMISSION = 0,	/* EACCES/EPERM from opendir/open/statx */
	SCAN_SKIP_UNREADABLE,		/* any other errno on the same paths */
	SCAN_SKIP_PATH_TOO_LONG,	/* over PATH_MAX, or a name over NAME_MAX */
	SCAN_SKIP_UNSUPPORTED_FS,	/* not btrfs/XFS, or a foreign fs */
	SCAN_SKIP_EXCLUDED,		/* --exclude matched */
	SCAN_SKIP_TOO_SMALL,		/* below --min-filesize */
	SCAN_SKIP_NOT_REGULAR,		/* neither a regular file nor a directory */
	SCAN_SKIP_READONLY_SUBVOL,	/* read-only btrfs subvolume, see #156 */
	SCAN_SKIP__COUNT
};

/* True for the buckets that indicate a problem rather than a config choice. */
bool scan_skip_is_error(enum scan_skip_bucket b);
/* Stable snake_case key for --json and the run_history columns. */
const char *scan_skip_key(enum scan_skip_bucket b);
/* Human phrase for the summary line, e.g. "unreadable (permission denied)". */
const char *scan_skip_desc(enum scan_skip_bucket b);

void filescan_count_skip(enum scan_skip_bucket b);
/* EACCES/EPERM land in PERMISSION, everything else in UNREADABLE. */
void filescan_count_errno_skip(int err);

/*
 * Snapshot the counters. Like pscan_files_scanned(), this must be read while
 * still inside scan_files(): the dedupe phase reuses the progress counters, and
 * keeping the two reads in one place stops the same trap being re-learned.
 */
void filescan_get_skips(uint64_t out[SCAN_SKIP__COUNT]);

/* For dbfile.c */
struct block_csum {
	uint64_t	loff;
	unsigned char	digest[DIGEST_LEN];
};

struct extent_csum {
	uint64_t	loff;
	uint64_t	poff;
	uint64_t	len;
	unsigned char	digest[DIGEST_LEN];
};

struct file_to_scan {
	char *path;
	int64_t fileid;
	size_t filesize;

	/*
	 * Used to record the current file position in the scan queue,
	 * to print the progress bar
	 */
	unsigned long long file_position;

	/* Intrusive FIFO link, owned by the scan work queue (file_scan.c). */
	struct file_to_scan *next;
};

int add_exclude_pattern(const char *pattern);
/* An exact path to skip, matched literally (the hashfile and its sidecars). */
void add_exclude_path(const char *path);
/* Warn about --exclude patterns that matched nothing; call after the walk. */
void filescan_report_excludes(void);

void filescan_init(void);
void filescan_free(void);

/*
 * Diagnostic csum-queue counters (DUPEREMOVE_SCAN_STATS): files dequeued by
 * workers and how many of those dequeues had to block on an empty queue
 * (workers starved by the single-threaded producer).
 */
void filescan_get_workq_stats(uint64_t *pops, uint64_t *empty_waits);

/*
 * Diagnostic ETA-calibration counters (DUPEREMOVE_SCAN_STATS): summed per-file
 * overhead (setup + finalize + DB write) and read+hash time, with their file
 * and byte counts. overhead/file over hash/byte is the ideal ETA file weight.
 */
void filescan_get_eta_calibration(uint64_t *overhead_ns, uint64_t *hash_ns,
				  uint64_t *files, uint64_t *bytes);

#endif	/* __FILE_SCAN_H__ */
