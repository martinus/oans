#ifndef	__FILE_SCAN_H__
#define	__FILE_SCAN_H__

#include <sys/types.h>
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
 * True if a file with this id was confirmed on disk during the last walk. Used
 * by the post-scan deleted-file prune to skip re-stat()ing files the scan
 * already visited. filescan_seen_reset() frees the backing bitset.
 */
bool filescan_file_was_seen(int64_t id);
void filescan_seen_reset(void);

void fs_get_locked_uuid(uuid_t *uuid);

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
};

int add_exclude_pattern(const char *pattern);

void filescan_init(void);
void filescan_free(void);

void add_file_fdupes(char *path);
#endif	/* __FILE_SCAN_H__ */
