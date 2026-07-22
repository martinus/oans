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

void filescan_init(void);
void filescan_free(void);

/*
 * Diagnostic csum-queue counters (DUPEREMOVE_SCAN_STATS): files dequeued by
 * workers and how many of those dequeues had to block on an empty queue
 * (workers starved by the single-threaded producer).
 */
void filescan_get_workq_stats(uint64_t *pops, uint64_t *empty_waits);

#endif	/* __FILE_SCAN_H__ */
