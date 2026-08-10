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
 * How many scan roots named on the command line (or in a "-" stdin list) could
 * not be resolved or stat()ed (#146). A typo, an unmounted path, a renamed
 * share or an accidentally-quoted glob all land here, and each means the run
 * covered less than it was asked to - so the caller reports it in the exit
 * status instead of exiting 0. Config-driven skips are deliberately excluded.
 */
unsigned int filescan_roots_unusable(void);

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
	SCAN_SKIP_TOO_LARGE,		/* above --max-filesize */
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

/*
 * True when the already-open `fd` lives in a read-only btrfs subvolume. One
 * ioctl per device, cached and genuinely shared with the walk's own lookups.
 * False for anything that is not a btrfs subvolume, and false by construction
 * whenever the walk is skipping read-only subvolumes (the default), since then
 * none can be in the hashfile to ask about.
 *
 * Used by the dedupe phase, which must never make such a file a dedupe
 * *destination*: FIDEDUPERANGE only ever rewrites the destination, so a
 * read-only member can legally be the source and nothing else (#171).
 */
bool filescan_fd_is_readonly_subvol(int fd);

/*
 * Release scan state that the dedupe phase still needs, so it cannot be freed
 * by filescan_free() while a second consumer is about to read it. Call once,
 * after the dedupe phase.
 */
void filescan_free_late(void);

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

/*
 * Where an earlier, interrupted run got to, and the running checksums as they
 * stood there (#159). All zero for the normal case of hashing a file from the
 * beginning: `csum` NULL means "there is nothing to resume".
 *
 * The checksums are restored on the listing thread, so a state this binary
 * cannot read is spotted while the decision to rescan from scratch is still
 * cheap to make. `ext_csum` is the digest of the extent that was mid-flight,
 * with ext_loff/ext_len naming it so the worker can confirm the layout has not
 * been rewritten since; NULL when the offset fell on an extent boundary.
 */
struct scan_resume {
	uint64_t off;
	struct running_checksum *csum;
	struct running_checksum *ext_csum;
	uint64_t ext_loff;
	uint64_t ext_len;
};

/* Release checksums nothing has taken ownership of yet. */
void scan_resume_drop(struct scan_resume *resume);

struct file_to_scan {
	char *path;
	int64_t fileid;
	size_t filesize;
	uint64_t mtime;		/* stamped into any checkpoint this file writes */

	/*
	 * NULL for the ordinary case of hashing from the start, which is every
	 * file in almost every run - hence a pointer rather than the struct
	 * inline. A scan queues one of these per listed file and the walk runs
	 * far ahead of the hashing, so this is 32 bytes per *listed* file of
	 * resident memory, ~17 MB on a half-million-file tree.
	 *
	 * Owned by whoever holds this file_to_scan, until the worker adopts the
	 * checksums into its scan context.
	 */
	struct scan_resume *resume;

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
 * Files whose hashes were copied from an already-scanned file with an identical
 * physical layout (#206), and the bytes not read because of it. Read after the
 * scan, like the skip counters.
 */
void filescan_get_layout_copies(uint64_t *files, uint64_t *bytes);
/* Bytes the donor table holds, for the RSS breakdown (#208). */
uint64_t filescan_layout_donor_bytes(void);

/*
 * Diagnostic ETA-calibration counters (DUPEREMOVE_SCAN_STATS): summed per-file
 * overhead (setup + finalize + DB write) and read+hash time, with their file
 * and byte counts. overhead/file over hash/byte is the ideal ETA file weight.
 */
void filescan_get_eta_calibration(uint64_t *overhead_ns, uint64_t *hash_ns,
				  uint64_t *files, uint64_t *bytes);

#endif	/* __FILE_SCAN_H__ */
