/*
 * file_scan.c
 *
 * Implementation of file scan and checksum phase.
 *
 * Copyright (C) 2014 SUSE.  All rights reserved.
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
 * Authors: Mark Fasheh <mfasheh@suse.de>
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <limits.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <linux/limits.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <inttypes.h>
#include <linux/magic.h>
#include <sys/statfs.h>
#include <fnmatch.h>
#include <blkid/blkid.h>
#include <libmount/libmount.h>
#include <sys/sysmacros.h>
#include <uuid/uuid.h>
#include <bsd/sys/queue.h>

#include <glib.h>

#include "csum.h"
#include "filerec.h"
#include "hash-tree.h"
#include "btrfs-util.h"
#include "debug.h"
#include "file_scan.h"
#include "dbfile.h"
#include "util.h"
#include "opt.h"
#include "threads.h"
#include "fiemap.h"
#include "progress.h"

/* This is not in linux/magic.h */
#ifndef	XFS_SB_MAGIC
#define	XFS_SB_MAGIC		0x58465342	/* 'XFSB' */
#endif

struct exclude_file {
	char *pattern;
	bool is_glob;	/* pattern has fnmatch metacharacters */
	SLIST_ENTRY(exclude_file) list;
};

SLIST_HEAD(exclude_list, exclude_file) exclude_head = SLIST_HEAD_INITIALIZER(exclude_head);

static int __scan_file(char *path, struct dbhandle *db, struct statx *st);
static bool seen_inode(uint64_t ino, uint64_t subvol);
static void mark_inode_seen(uint64_t ino, uint64_t subvol);

static struct threads_pool scan_pool;

/*
 * Scan-phase batched writer.
 *
 * Every hashfile write is serialized behind the write lock (dbfile_lock()):
 * our sqlite connections use SQLITE_OPEN_NOMUTEX and WAL only permits a single
 * writer. Committing one transaction per file therefore dominates the scan of
 * a large tree - each commit forces its own WAL frames and fcntl locking,
 * funnelled through that single writer, and extra io-threads just pile up on
 * the lock.
 *
 * So the scan routes every write (the file-record upsert while listing and the
 * hash store while csumming) through one dedicated connection and keeps a
 * single transaction open across many files, committing once every
 * WRITE_BATCH_FILES. Reads keep using their own connections, so read
 * concurrency is unchanged.
 *
 * scan_write_{begin,end,abort}() must be called with the write lock held.
 * scan_writer_{open,close}() bracket the scan while no worker is running.
 */
#define COMMIT_INTERVAL_SEC	10.0
static struct dbhandle *scan_writer;
static bool scan_trans_open;
static double scan_write_start;
static struct dbhandle *scan_read_db;	/* listing handle whose reads we batch */
static bool scan_read_open;
static double scan_read_start;

static int scan_writer_open(void)
{
	scan_writer = dbfile_open_handle(options.hashfile);
	return scan_writer ? 0 : -1;
}

/* Ensure a batch transaction is open. Call with the write lock held. */
static int scan_write_begin(void)
{
	int ret;

	if (scan_trans_open)
		return 0;

	ret = dbfile_begin_trans(scan_writer->db);
	if (ret)
		return ret;

	scan_trans_open = true;
	scan_write_start = elapsed_seconds();
	return 0;
}

/* Commit any open batch. Call with the write lock held. */
static int scan_write_flush(void)
{
	int ret;

	if (!scan_trans_open)
		return 0;

	ret = dbfile_commit_trans(scan_writer->db);
	scan_trans_open = false;
	return ret;
}

/* Commit the write batch once it has been open COMMIT_INTERVAL_SEC. */
static int scan_write_end(void)
{
	if (scan_trans_open && elapsed_seconds() - scan_write_start >= COMMIT_INTERVAL_SEC)
		return scan_write_flush();
	return 0;
}

/* Roll back the current batch. Call with the write lock held. */
static void scan_write_abort(void)
{
	if (!scan_trans_open)
		return;

	dbfile_abort_trans(scan_writer->db);
	scan_trans_open = false;
}

/* Commit the listing read transaction if one is open. */
static void scan_read_flush(void)
{
	if (scan_read_open) {
		dbfile_commit_trans(scan_read_db->db);
		scan_read_open = false;
	}
}

/*
 * Keep one read transaction open across the per-file change-detection lookups,
 * refreshed on the COMMIT_INTERVAL_SEC cadence so the reader snapshot doesn't
 * pin the WAL against checkpointing. Listing thread only, so no locking.
 */
static void scan_read_tick(struct dbhandle *db)
{
	double now = elapsed_seconds();

	scan_read_db = db;
	if (scan_read_open && now - scan_read_start >= COMMIT_INTERVAL_SEC)
		scan_read_flush();
	if (!scan_read_open && dbfile_begin_trans(db->db) == 0) {
		scan_read_open = true;
		scan_read_start = now;
	}
}

/* Flush and drop the writer. Call while no worker thread is running. */
static void scan_writer_close(void)
{
	if (!scan_writer)
		return;

	dbfile_lock();
	scan_write_flush();
	dbfile_unlock();

	dbfile_close_handle(scan_writer);
	scan_writer = NULL;
}

#define READ_BUF_LEN (8*1024*1024) // 8MB

struct buffer {
	char *buf;
	size_t size; /* Size of buf */

	/*
	 * Data has been processed up to this offset
	 * Whatever is afterward should be move at the begining of buf
	 * and not thrown away.
	 */
	size_t dl_offset;

	/* Size of the unprocessed data left in the buf */
	size_t dl_len;

	/* Set to true if the buffer is zeroed */
	bool faked;
};

/*
 * A structure to keep our file hashes before committing them
 * to the hash table
 * extents_count and blocks_count are the size of the allocated arrays
 * extents_index and blocks_index are the index of the next free entries
 */
struct hashes {
	unsigned int extents_count;
	unsigned int extents_index;
	struct extent_csum *extents;

	unsigned int blocks_count;
	unsigned int blocks_index;
	struct block_csum *blocks;
};

struct scan_ctxt {
	int fd;
	size_t filesize;
	size_t off; /* file offset of the last processed bytes */
	struct fiemap *fiemap;
	unsigned int extent_cursor; /* resume hint for get_extent() in process_extents */
	struct running_checksum *file_csum;
	struct running_checksum *extent_csum;
};

/*
 * Represents the filesystem we are working on
 * Its UUID may be found in the hashfile
 * The dev_t may change at each run, so we discover its
 * value at runtime and use it to quicken the check on non-btrfs fs
 */
struct locked_fs {
	uuid_t uuid;
	dev_t dev;
	bool is_btrfs;
};
struct locked_fs locked_fs = {0,};

static bool allocate_hashes(struct hashes *hashes, struct scan_ctxt *ctxt)
{
	hashes->extents_count = ctxt->fiemap->fm_mapped_extents;
	hashes->extents = calloc(hashes->extents_count, sizeof(struct extent_csum));

	hashes->blocks_count = ctxt->filesize / blocksize + 1;
	hashes->blocks = calloc(hashes->blocks_count, sizeof(struct block_csum));

	return hashes->extents && hashes->blocks;
}

static void free_hashes(struct hashes *hashes)
{
	if (!hashes)
		return;

	if (hashes->extents)
		free(hashes->extents);

	if (hashes->blocks)
		free(hashes->blocks);
}

static int prepare_buffer(struct buffer *buffer)
{
	if (!buffer)
		goto err;

	memset(buffer, 0, sizeof(struct buffer));
	buffer->buf = calloc(1, READ_BUF_LEN);

	if (!(buffer->buf))
		goto err;

	buffer->size = READ_BUF_LEN;

	register_cleanup(&scan_pool, (void*)&free, buffer->buf);
	return 0;

err:
	eprintf("prepare_buffer failed\n");
	return 1;
}

static void free_scan_ctxt(struct scan_ctxt *ctxt)
{
	if (!ctxt)
		return;

	if (ctxt->fd >= 0)
		close(ctxt->fd);

	if (ctxt->fiemap)
		free(ctxt->fiemap);

	if (ctxt->file_csum)
		finish_running_checksum(ctxt->file_csum, NULL);

	if (ctxt->extent_csum)
		finish_running_checksum(ctxt->extent_csum, NULL);
}

static int is_excluded(const char *name)
{
	struct exclude_file *exclude;

	SLIST_FOREACH(exclude, &exclude_head, list) {
		/*
		 * Patterns without metacharacters (e.g. the hashfile paths we
		 * always exclude) match exactly, so skip fnmatch's much more
		 * expensive char-by-char loop - this runs for every file.
		 */
		bool match = exclude->is_glob ?
			(fnmatch(exclude->pattern, name, 0) == 0) :
			(strcmp(exclude->pattern, name) == 0);
		if (match) {
			vprintf("Excluding: %s (matches %s)\n", name,
				exclude->pattern);
			return 1;
		}
	}

	return 0;
}

static inline void mnt_unref_table_cleanup(struct libmnt_table **tb)
{
	if (tb && *tb)
		mnt_unref_table(*tb);
}

static inline dev_t stx_to_dev(struct statx *stx)
{
	return makedev(stx->stx_dev_major, stx->stx_dev_minor);
}

/*
 * Cache of btrfs subvolume ids, keyed by device.
 *
 * btrfs assigns a distinct anonymous st_dev to every subvolume, and
 * lookup_btrfs_subvol() returns the same tree id for every file within a
 * subvolume. So rather than open()+ioctl() on each individual file just to
 * learn its subvolume, we do it once per subvolume and reuse the result for
 * every later file on the same device.
 *
 * Touched only from the single __scan_file() consumer (not the walker threads),
 * so no locking is needed.
 */
struct subvol_cache_entry {
	dev_t				dev;
	uint64_t			subvol;
	struct subvol_cache_entry	*next;
};
static struct subvol_cache_entry *subvol_cache;

static bool subvol_cache_get(dev_t dev, uint64_t *subvol)
{
	struct subvol_cache_entry *e;

	for (e = subvol_cache; e; e = e->next) {
		if (e->dev == dev) {
			*subvol = e->subvol;
			return true;
		}
	}
	return false;
}

static void subvol_cache_put(dev_t dev, uint64_t subvol)
{
	struct subvol_cache_entry *e = malloc(sizeof(*e));

	/* On OOM just skip caching; the caller still has a valid subvol. */
	if (!e)
		return;

	e->dev = dev;
	e->subvol = subvol;
	e->next = subvol_cache;
	subvol_cache = e;
}

static void subvol_cache_free(void)
{
	struct subvol_cache_entry *e = subvol_cache, *next;

	while (e) {
		next = e->next;
		free(e);
		e = next;
	}
	subvol_cache = NULL;
}

/*
 * Cache of devices already confirmed to belong to the locked filesystem.
 *
 * On btrfs check_file() verifies each directory lives on the locked fs by
 * comparing its fs UUID, because subvolumes have distinct st_dev values so a
 * plain device compare can't span them. That costs an open()+statfs()+ioctl per
 * directory. Since the answer is stable per device, we - like the subvolume
 * cache above - remember each confirmed device and skip the recheck for every
 * later directory on the same subvolume. Listing thread only, so no locking.
 */
struct verified_dev_entry {
	dev_t				dev;
	struct verified_dev_entry	*next;
};
static struct verified_dev_entry *verified_devs;
/* check_file() runs on the parallel walker threads, so this cache is shared. */
static GMutex verified_dev_lock;

static bool verified_dev_get(dev_t dev)
{
	struct verified_dev_entry *e;
	bool found = false;

	g_mutex_lock(&verified_dev_lock);
	for (e = verified_devs; e; e = e->next)
		if (e->dev == dev) {
			found = true;
			break;
		}
	g_mutex_unlock(&verified_dev_lock);
	return found;
}

static void verified_dev_put(dev_t dev)
{
	struct verified_dev_entry *e = malloc(sizeof(*e));

	/* On OOM just skip caching; correctness is unaffected. */
	if (!e)
		return;

	e->dev = dev;
	g_mutex_lock(&verified_dev_lock);
	e->next = verified_devs;
	verified_devs = e;
	g_mutex_unlock(&verified_dev_lock);
}

static void verified_dev_free(void)
{
	struct verified_dev_entry *e = verified_devs, *next;

	while (e) {
		next = e->next;
		free(e);
		e = next;
	}
	verified_devs = NULL;
}

static char *extract_first_device(const char *fs_source)
{
	char *first_device = NULL;
	const char *colon;

	if (!fs_source)
		return NULL;

	colon = strchr(fs_source, ':');
	if (colon) {
		size_t len = colon - fs_source;
		first_device = malloc(len + 1);
		if (!first_device)
			return NULL;
		memcpy(first_device, fs_source, len);
		first_device[len] = '\0';
	} else {
		first_device = strdup(fs_source);
		if (!first_device)
			return NULL;
	}
	return first_device;
}

/* Get the UUID associated with the FS that stores path */
int get_uuid(char *path, uuid_t *uuid)
{
	struct statx st;
	int ret;
	_cleanup_(mnt_unref_table_cleanup) struct libmnt_table *tb = NULL;
	_cleanup_(closefd) int fd = open(path, O_RDONLY);
	_cleanup_(freep) char *uuid_found = NULL;

	struct libmnt_fs *dev = NULL;

	if (fd == -1) {
		eprintf("Cannot open %s: %s\n", path, strerror(errno));
		return 1;
	}

	if (is_btrfs(path)) {
		dprintf("get_uuid: %s lives on btrfs\n", path);
		ret = btrfs_get_fsuuid(fd, uuid);
		if (ret) {
			eprintf("%s: btrfs_get_fsuuid failed\n",
				path);
			return 1;
		}
	} else {
		const char *fs_source;
		char *first_device;

		dprintf("get_uuid: %s do not live on btrfs\n", path);

		ret = statx(0, path, 0, STATX_BASIC_STATS, &st);
		if (ret) {
			eprintf("Failed to stat %s: %s\n",
					path, strerror(errno));
			return 1;
		}

		if (st.stx_dev_major == 0) {
			dprintf("%s lives on an unsupported filesystem, skipping. "
				"Please fill a bug if you think this is a mistake.\n",
					path);
			return 1;
		}

		tb = mnt_new_table_from_file("/proc/self/mountinfo");
		if (!tb) {
			perror("unable to read and parse /proc/self/mountinfo");
			return 1;
		}

		dev = mnt_table_find_devno(tb, stx_to_dev(&st), MNT_ITER_FORWARD);
		if (!dev) {
			eprintf("%s: unable to find the mount infos\n",
					path);
			return 1;
		}

		fs_source = mnt_fs_get_source(dev);
		first_device = extract_first_device(fs_source);

		if (!first_device) {
			eprintf("Memory allocation failed\n");
			return 1;
		}

		uuid_found = blkid_get_tag_value(NULL, "UUID", first_device);
		free(first_device);
		if (!uuid_found) {
			eprintf("libblkid could not get uuid for "
					"device %s. Run blkid as root to "
					"populate the cache.\n",
					mnt_fs_get_source(dev));
			return 1;
		}

		uuid_parse(uuid_found, *uuid);
	}
	return 0;
}

static inline uint64_t timestamp_to_nano(struct statx_timestamp t)
{
	return t.tv_sec * 1000000000 + t.tv_nsec;
}

/*
 * Check if path lives on a filesystem that is supported, eg
 * that is known to support deduplication.
 */
bool is_fs_supported(char *path)
{
	struct statfs fs;
	int ret;

	ret = statfs(path, &fs);
	if (ret) {
		eprintf("Error %d: %s while check fs type on %s",
			errno, strerror(errno), path);
		return false;
	}

	return (fs.f_type == BTRFS_SUPER_MAGIC ||
		fs.f_type == XFS_SB_MAGIC);
}

/* Check if path should be processed:
 * - is path not excluded ?
 * - is path a file or directory ?
 * - is path at least --min-filesize bytes (empty files by default) ?
 * - does path lives on our locked filesystem ?
 *   for files, we only do that check if the parent is not checked
 *
 * Returns true is the file is legit, false if not (or on error)
 */
bool check_file(struct dbhandle *db, char *path, struct statx *st, bool parent_checked)
{
	int ret;
	struct dbfile_config cfg;
	uuid_t uuid = {0,};
	dev_t dev;

	if (is_excluded(path))
		return false;

	if (!S_ISREG(st->stx_mode) && !S_ISDIR(st->stx_mode)) {
		vprintf("Skipping non-regular/non-directory file %s\n", path);
		return false;
	}

	if (S_ISREG(st->stx_mode) && st->stx_size < options.min_filesize) {
		vprintf("Skipping file below --min-filesize: %s (%llu < %"PRIu64")\n",
			path, st->stx_size, options.min_filesize);
		return false;
	}

	/* There is no need to check if the file lives in our locked fs.
	 * It is a regular file and we already check its parent.
	 */
	if (S_ISREG(st->stx_mode) && parent_checked)
		return true;

	/* Locked-fs checks */
	/* First, try to get uuid from the hashfile */
	if (uuid_is_null(locked_fs.uuid)) {
		dprintf("Looking our fs uuid from the hashfile\n");
		ret = dbfile_get_config(db->db, &cfg);
		if (ret)
			return 1;

		if (!uuid_is_null(cfg.fs_uuid))
			uuid_copy(locked_fs.uuid, cfg.fs_uuid);
	}

	/* hashfile was empty. We lock on the file. */
	if (uuid_is_null(locked_fs.uuid)) {
		dprintf("Empty hashfile, locking on the current file\n");
		ret = get_uuid(path, &locked_fs.uuid);
		if (ret)
			return false;

		locked_fs.dev = stx_to_dev(st);
		locked_fs.is_btrfs = is_btrfs(path);

		if (!is_fs_supported(path))
			eprintf("Warn: filesystem for %s is not known to "
				"support deduplication.\n", path);

		return true;
	}

	/* Hashfile was not empty */
	/* We miss runtime data, check if our fille is in the valid fs
	 * and store them for future calls
	 */
	if (locked_fs.dev == 0) {
		ret = get_uuid(path, &uuid);
		if (ret)
			return false;

		if (uuid_compare(uuid, locked_fs.uuid) != 0) {
			eprintf("%s lives on fs ", path);
			debug_print_uuid(uuid);
			eprintf(" will we are locked on fs ");
			debug_print_uuid(locked_fs.uuid);
			eprintf(".\n");
			return false;
		}

		locked_fs.dev = stx_to_dev(st);
		locked_fs.is_btrfs = is_btrfs(path);
		return true;
	}

	if (!locked_fs.is_btrfs)
		return locked_fs.dev == stx_to_dev(st);

	/*
	 * On btrfs each subvolume has a distinct st_dev, so verify by fs UUID
	 * rather than by device. That costs an open()+statfs()+ioctl, so cache
	 * devices already confirmed to be on the locked fs and skip the recheck.
	 */
	dev = stx_to_dev(st);
	if (verified_dev_get(dev))
		return true;

	ret = get_uuid(path, &uuid);
	if (ret)
		return false;

	if (uuid_compare(uuid, locked_fs.uuid) != 0)
		return false;

	verified_dev_put(dev);
	return true;
}

void fs_get_locked_uuid(uuid_t *uuid)
{
	if (uuid)
		uuid_copy(*uuid, locked_fs.uuid);
}

static int get_dirent_type(struct dirent *entry, int fd, const char *path)
{
	int ret;
	struct statx st;

	if (entry->d_type != DT_UNKNOWN)
		return entry->d_type;

	/*
	 * FS doesn't support file type in dirent, do this the old
	 * fashioned way. We translate mode to DT_* for the
	 * convenience of the caller.
	 */
	ret = statx(fd, entry->d_name, AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS, &st);
	if (ret || !(st.stx_mask & STATX_BASIC_STATS)) {
		eprintf("Error %d: %s while getting type of file %s/%s. "
			"Skipping.\n",
			errno, strerror(errno), path, entry->d_name);
		return DT_UNKNOWN;
	}

	if (S_ISREG(st.stx_mode))
		return DT_REG;
	if (S_ISDIR(st.stx_mode))
		return DT_DIR;
	if (S_ISBLK(st.stx_mode))
		return DT_BLK;
	if (S_ISCHR(st.stx_mode))
		return DT_CHR;
	if (S_ISFIFO(st.stx_mode))
		return DT_FIFO;
	if (S_ISLNK(st.stx_mode))
		return DT_LNK;
	if (S_ISSOCK(st.stx_mode))
		return DT_SOCK;

	return DT_UNKNOWN;
}

/*
 * Returns nonzero on fatal errors only
 */
/*
 * Parallel directory walk.
 *
 * Walker threads traverse the tree - opendir/readdir/statx plus the
 * directory-level check_file() - which is the listing cost and scales nearly
 * linearly across cores. Every regular file they find is handed to a single
 * consumer (the main thread) that runs __scan_file() exactly as the serial code
 * did, so all the DB write / dedupe_seq / seen_inodes / batched-read logic stays
 * single-threaded and untouched.
 *
 * locked_fs is initialised by the main thread while seeding the roots (before
 * any walker starts), so the walkers only read it; the one cache they share,
 * verified_devs, is locked. Each walker gets its own db handle so a stray
 * check_file() config read never races on a shared connection.
 */
struct scan_item {
	struct statx	st;
	char		path[];		/* NUL-terminated path follows */
};

#define WALK_STOP	((void *)1)	/* queue sentinel */

static GAsyncQueue	*walk_dirq;	/* char* directories to visit */
static GAsyncQueue	*walk_fileq;	/* struct scan_item* for the consumer */
/*
 * Outstanding directories, plus a +1 "seeding" token held until
 * filescan_walk_run() releases it, so the count can't hit zero while roots are
 * still being queued. process_dir() enqueues a directory's files before
 * dirq_finished() decrements, so when this reaches zero every file has been
 * queued: we then stop the walkers and tell the consumer no more are coming.
 */
static gint		walk_dir_pending;
static unsigned int	walk_nthreads;

static void dirq_stop_walkers(void)
{
	unsigned int i;

	for (i = 0; i < walk_nthreads; i++)
		g_async_queue_push(walk_dirq, WALK_STOP);
	g_async_queue_push(walk_fileq, WALK_STOP);
}

/* Queue a directory for the walkers. Takes ownership of path. */
static void dirq_push(char *path)
{
	g_atomic_int_inc(&walk_dir_pending);
	g_async_queue_push(walk_dirq, path);
}

/* Called when a directory is done (or to release the seeding token). */
static void dirq_finished(void)
{
	if (g_atomic_int_dec_and_test(&walk_dir_pending))
		dirq_stop_walkers();
}

/* Hand a regular file to the consumer. */
static void fileq_push(const char *path, struct statx *st)
{
	size_t n = strlen(path) + 1;
	struct scan_item *it = malloc(sizeof(*it) + n);

	if (!it) {
		eprintf("scan: out of memory queuing %s\n", path);
		return;
	}
	it->st = *st;
	memcpy(it->path, path, n);
	g_async_queue_push(walk_fileq, it);
}

/* Read one directory: queue subdirs, hand regular files to the consumer. */
static void process_dir(const char *path, struct dbhandle *db)
{
	struct dirent *entry;
	struct statx st;
	_cleanup_(closedirectory) DIR *dirp = opendir(path);
	char child[PATH_MAX + 257] = { 0, };

	if (dirp == NULL) {
		eprintf("Error %d: %s while opening directory %s\n",
			errno, strerror(errno), path);
		return;
	}

	while (true) {
		errno = 0;
		entry = readdir(dirp);
		if (!entry) {
			if (errno)
				eprintf("Error %d: %s while reading directory %s\n",
					errno, strerror(errno), path);
			break;
		}

		if (strcmp(entry->d_name, ".") == 0
		    || strcmp(entry->d_name, "..") == 0)
			continue;

		entry->d_type = get_dirent_type(entry, dirfd(dirp), path);

		if (entry->d_type != DT_REG &&
		    !(options.recurse_dirs && entry->d_type == DT_DIR))
			continue;

		if (strlen(path) + 1 + strlen(entry->d_name) > PATH_MAX)
			continue;

		if (strcmp(path, "/") == 0)
			sprintf(child, "/%s", entry->d_name);
		else
			sprintf(child, "%s/%s", path, entry->d_name);

		if (statx(0, child, 0, STATX_BASIC_STATS, &st) ||
		    !(st.stx_mask & STATX_BASIC_STATS)) {
			eprintf("Failed to stat %s: %s\n", child, strerror(errno));
			continue;
		}

		if (!check_file(db, child, &st, true))
			continue;

		if (entry->d_type == DT_REG)
			fileq_push(child, &st);
		else
			dirq_push(strdup(child));
	}
}

static gpointer walk_thread(gpointer arg)
{
	struct dbhandle *db = arg;	/* this walker's own read handle */

	for (;;) {
		char *path = g_async_queue_pop(walk_dirq);

		if (path == WALK_STOP)
			break;
		process_dir(path, db);
		free(path);
		dirq_finished();
	}

	dbfile_close_handle(db);
	return NULL;
}

/* Set up the walk queues. Call before seeding roots via scan_file(). */
void filescan_walk_begin(void)
{
	walk_nthreads = options.io_threads ? options.io_threads : 1;
	walk_dirq = g_async_queue_new();
	walk_fileq = g_async_queue_new();
	walk_dir_pending = 1;	/* seeding token; released by filescan_walk_run() */
}

/*
 * Start the walkers and consume every file they find on the current thread.
 * The roots have already been seeded (scan_file), so locked_fs is set.
 */
int filescan_walk_run(struct dbhandle *db)
{
	GThread **threads = calloc(walk_nthreads, sizeof(*threads));
	unsigned int i;
	int ret = 0;

	abort_on(!threads);

	for (i = 0; i < walk_nthreads; i++) {
		struct dbhandle *wdb = dbfile_open_handle(options.hashfile);

		abort_on(!wdb);
		threads[i] = g_thread_new("walker", walk_thread, wdb);
	}

	/*
	 * Release the seeding token now the roots are queued. If nothing was
	 * queued this drops the count to zero and stops the walkers at once;
	 * otherwise the last directory to finish does it.
	 */
	dirq_finished();

	/* Consumer: single-threaded __scan_file() for every file found. */
	for (;;) {
		struct scan_item *it = g_async_queue_pop(walk_fileq);

		if (it == WALK_STOP)
			break;
		if (!ret)
			ret = __scan_file(it->path, db, &it->st);
		free(it);
	}

	for (i = 0; i < walk_nthreads; i++)
		g_thread_join(threads[i]);
	free(threads);
	g_async_queue_unref(walk_dirq);
	g_async_queue_unref(walk_fileq);
	walk_dirq = walk_fileq = NULL;
	return ret;
}

static inline bool is_file_renamed(char *path_in_db, char *path)
{
	struct stat st;

	if (strlen(path_in_db) == 0 || strcmp(path_in_db, path) == 0)
		return false;

	/*
	 * Old path and new paths differs. Could be hardlink,
	 * so we check if the old still exists.
	 */
	return lstat(path_in_db, &st);
}

/*
 * Returns nonzero on fatal errors only
 * This function schedules csum_whole_file()
 * The caller must call check_file() before and must not call
 * this if path is not a regular file.
 */
static int __scan_file(char *path, struct dbhandle *db, struct statx *st)
{
	int ret;
	struct file dbfile = {0,};
	static unsigned int seq = 0, counter = 0;
	GError *err = NULL;
	struct file_to_scan *file;
	int64_t fileid = 0;
	bool file_renamed;
	static uint64_t position = 0;

	/*
	 * The first call initializes the static variable
	 * from the global dedupe_seq
	 * The subsequents calls will increase it every <batchsize> times
	 */
	if (seq == 0)
		seq = dedupe_seq + 1;

	abort_on(!S_ISREG(st->stx_mode));

	pscan_examined();	/* count every file the listing walk visits */
	scan_read_tick(db);

	if (locked_fs.is_btrfs && !subvol_cache_get(stx_to_dev(st), &dbfile.subvol)) {
		_cleanup_(closefd) int fd;
		fd = open(path, O_RDONLY);
		if (fd == -1) {
			eprintf("Error %d: %s while opening file \"%s\". "
				"Skipping.\n", errno, strerror(errno), path);
			return 0;
		}

		/*
		 * Inodes between subvolumes on a btrfs file system
		 * can have the same i_ino. Get the subvolume id of
		 * our file so hard link detection works. This is constant
		 * within a subvolume (one st_dev), so cache it per device.
		 */
		ret = lookup_btrfs_subvol(fd, &(dbfile.subvol));
		if (ret) {
			eprintf("Error %d: %s while finding subvol for file "
				"\"%s\". Skipping.\n", ret, strerror(ret),
				path);
			return 0;
		}

		subvol_cache_put(stx_to_dev(st), dbfile.subvol);
	}

	/*
	 * Another hardlink to an inode we already wrote this scan. Its filerec
	 * is pending in the uncommitted batch and thus invisible to the read
	 * connection below, so re-storing it would corrupt the batch (see
	 * seen_inodes). One filerec per inode is enough, so skip it.
	 */
	if (seen_inode(st->stx_ino, dbfile.subvol))
		return 0;

	/*
	 * Check the database to see if that file need rescan or not.
	 */
	ret = dbfile_describe_file(db, st->stx_ino, dbfile.subvol, &dbfile);
	if (ret) {
		vprintf("dbfile_describe_file failed\n");
		return 0;
	}

	file_renamed = is_file_renamed(dbfile.filename, path);

	/* Database is up-to-date, nothing more to do */
	if (dbfile.mtime == timestamp_to_nano(st->stx_mtime)
	    && dbfile.size == st->stx_size && !file_renamed)
		return 0;

	if (options.batch_size != 0) {
		counter += 1;
		if (counter >= options.batch_size) {
			seq++;
			counter = 0;
		}
	}

	dbfile.ino = st->stx_ino;
	dbfile.size = st->stx_size;
	strncpy(dbfile.filename, path, PATH_MAX);
	dbfile.mtime = timestamp_to_nano(st->stx_mtime);
	dbfile.dedupe_seq = seq;

	/* Reads use the caller's handle; all writes go to the scan writer. */
	struct dbhandle *wdb = scan_writer;

	dbfile_lock();
	ret = scan_write_begin();
	if (ret) {
		dbfile_unlock();
		return 0;
	}

	if (file_renamed) {
		ret = dbfile_rename_file(wdb, dbfile.id, path);
		if (ret) {
			vprintf("dbfile_rename_file failed\n");
			dbfile_unlock();
			return 0;
		}
	}

	if (dbfile.mtime != 0 || dbfile.size != 0) {
		/*
		 * The file was scanned in a previous run.
		 * We will rescan it, so let's remove old hashes
		 */
		dbfile_remove_hashes(wdb, dbfile.id);
	}

	/* Upsert the file record */
	fileid = dbfile_store_file_info(wdb, &dbfile);
	if (!fileid) {
		scan_write_abort();
		dbfile_unlock();
		return 0;
	}

	scan_write_end();
	dbfile_unlock();

	/* Remember this inode so later hardlinks to it are skipped. */
	mark_inode_seen(dbfile.ino, dbfile.subvol);

	/* Schedule the file for scan */
	file = malloc(sizeof(struct file_to_scan)); /* Freed by csum_whole_file() */

	file->path = strdup(path);
	file->fileid = fileid;
	file->filesize = st->stx_size;

	pscan_set_progress(1, st->stx_size);
	position++;
	file->file_position = position;

	if(!g_thread_pool_push(scan_pool.pool, file, &err)) {
		eprintf("g_thread_pool_push: %s\n", err->message);
		g_error_free(err);
		err = NULL;
		free(file);
		return 1;
	}

	return 0;
}

/* The entry point for files passed by the user */
int scan_file(char *in_path, struct dbhandle *db)
{
	struct statx st;
	char path[PATH_MAX];
	int ret;

	/*
	 * Sanitize the file name and get absolute path. This avoids:
	 *
	 * - needless filerec writes to the db when we have
	 *   effectively the same filename but the components have extra '/'
	 *
	 * - Absolute path allows the user to re-run this hash from
	 *   any directory.
	 */
	if (realpath(in_path, path) == NULL) {
		eprintf("Error %d: %s while getting path to file %s. "
			"Skipping.\n",
			errno, strerror(errno), in_path);
		return 0;
	}

	ret = statx(0, path, 0, STATX_BASIC_STATS, &st);
	if (ret || !(st.stx_mask & STATX_BASIC_STATS)) {
		eprintf("Error %d: %s while stating file %s. "
			"Skipping.\n",
			errno, strerror(errno), path);
		return 0;
	}

	/*
	 * Seed the parallel walk. check_file() here runs on the main thread and
	 * locks onto the target filesystem (initialising locked_fs) before any
	 * walker starts. Regular files go straight to the consumer queue;
	 * directories are handed to the walker pool. filescan_walk_run() then
	 * does the actual traversal and scanning.
	 */
	if (!check_file(db, path, &st, false))
		return 0;

	if (S_ISREG(st.stx_mode))
		fileq_push(path, &st);
	else
		dirq_push(strdup(path));
	return 0;
}

/* Check if the block starting at buf is full of zeroes */
static inline int is_block_zeroed(void *buf)
{
	return buf && ((int*)buf)[0] == 0 && !memcmp(buf, buf + 1, blocksize - 1);
}

static int add_block_hash(struct hashes *hashes,
			  uint64_t loff, unsigned char *digest)
{
	struct block_csum *retp;

	if (hashes->blocks_index + 1 > hashes->blocks_count) {
		/* Somehow, we did not allocate enough memory */
		hashes->blocks_count++;
		retp = realloc(hashes->blocks, sizeof(struct block_csum) * hashes->blocks_count);
		if (!retp)
			return -ENOMEM;
		hashes->blocks = retp;
	}

	hashes->blocks[hashes->blocks_index].loff = loff;
	memcpy(hashes->blocks[hashes->blocks_index].digest, digest, DIGEST_LEN);
	hashes->blocks_index++;
	return 0;
}

/*
 * Check if the area should be scanned.
 */
static bool is_area_ignored(struct fiemap *fiemap, size_t start, size_t len)
{
	size_t end = start + len;
	struct fiemap_extent *current_extent;
	while (start < end) {
		current_extent = get_extent(fiemap, start, NULL);

		/* File changed since we fiemap */
		if (!current_extent)
			return false;

		if (current_extent->fe_flags & FIEMAP_SKIP_FLAGS)
			return true;

		if (current_extent->fe_flags & FIEMAP_EXTENT_LAST)
			break;
		start = current_extent->fe_logical + current_extent->fe_length + 1;
	}
	return false;
}

/*
 * Check if the block starting at off should be ignored.
 */
static inline bool is_block_ignored(struct fiemap *fiemap, size_t off)
{
	return is_area_ignored(fiemap, off, blocksize);
}

static int process_block(char *buf, unsigned int bsize,
		size_t file_off, struct hashes *hashes)
{
	unsigned char digest[DIGEST_LEN];
	checksum_block(buf, bsize, digest);
	return add_block_hash(hashes, file_off, digest);
}

/*
 * Processes entire blocks from buffer.
 * Partial blocks are ignored: the buffer needs to be refilled.
 * Returns the total of bytes processed.
*/
static ssize_t process_blocks(struct scan_ctxt *ctxt, struct buffer *buffer,
			      struct hashes *hashes)
{
	int ret = 0;
	unsigned int nb_blocks = buffer->dl_len / blocksize;
	size_t curr_file_off = ctxt->off;

	/* We do not actually need to process the blocks */
	if (!options.do_block_hash || buffer->faked)
		return buffer->dl_len;

	for (unsigned int i = 0; i < nb_blocks; i++) {
		if (!is_block_ignored(ctxt->fiemap, curr_file_off) &&
		    !(options.skip_zeroes &&
		      is_block_zeroed(buffer->buf + i * blocksize))) {
			ret = process_block(buffer->buf + i * blocksize,
					    blocksize, curr_file_off, hashes);
			if (ret)
				return ret;
		}

		curr_file_off += blocksize;
	}

	return nb_blocks * blocksize;
}

static int store_extent(struct scan_ctxt *ctxt, struct hashes *hashes, struct fiemap_extent *extent)
{
	struct extent_csum *retp;

	if (hashes->extents_index + 1 > hashes->extents_count) {
		/* Somehow, we did not allocate enough memory */
		hashes->extents_count++;
		retp = realloc(hashes->extents, sizeof(struct extent_csum) * hashes->extents_count);
		if (!retp)
			return -ENOMEM;
		hashes->extents = retp;
	}

	if (extent->fe_flags & FIEMAP_SKIP_FLAGS) {
		hashes->extents[hashes->extents_index].len = 0;
	} else {
		hashes->extents[hashes->extents_index].loff = extent->fe_logical;
		hashes->extents[hashes->extents_index].poff = extent->fe_physical;
		hashes->extents[hashes->extents_index].len  = extent->fe_length;
		finish_running_checksum(ctxt->extent_csum, hashes->extents[hashes->extents_index].digest);
		ctxt->extent_csum = NULL;
	}
	hashes->extents_index++;

	return 0;
}

static int process_extents(struct scan_ctxt *ctxt, struct buffer *buffer,
			   struct hashes *hashes, size_t bytes)
{
	/* Local variables to not overwrite the context etc */
	size_t file_off = ctxt->off;
	size_t buf_off = 0;

	int ret;
	struct fiemap_extent *extent;
	size_t ext_end_off;
	size_t to_add;

	while (file_off < ctxt->off + bytes) {
		extent = get_extent(ctxt->fiemap, file_off, &ctxt->extent_cursor);
		if (!extent) {
			eprintf("process_extents: unable to get extent\n");

			/* Cleanup the partial checksum and skip
			 * the rest of the buffer
			 */
			if (ctxt->extent_csum)
				finish_running_checksum(ctxt->extent_csum, NULL);
			ctxt->extent_csum = NULL;
			return 1;
		}

		ext_end_off = extent->fe_logical + extent->fe_length;

		if (ext_end_off > ctxt->off + bytes)
			/* Extent ends after our buffer */
			to_add = bytes - buf_off;
		else
			to_add = ext_end_off - file_off;

		if (!(extent->fe_flags & FIEMAP_SKIP_FLAGS)) {
			if (ctxt->extent_csum == NULL) {
				ctxt->extent_csum = start_running_checksum();
			}

			add_to_running_checksum(ctxt->extent_csum, (unsigned char*)buffer->buf + buf_off, to_add);
		}

		assert(file_off + to_add <= ctxt->off + bytes);

		buf_off += to_add;
		file_off += to_add;

		/*
		 * ext_end_off may be 4k-aligned:
		 * Unless FIEMAP_EXTENT_NOT_ALIGNED is returned,
		 * fe_logical, fe_physical, and fe_length will be aligned
		 * to the block size of the file system.
		 * So, if we are processing the last extent, then
		 * ext_end_off may be larger than the filesize. For those extents, add
		 * the part that will never exist.
		 */
		size_t dummy = 0;
		if (extent->fe_flags & FIEMAP_EXTENT_LAST)
			dummy = ext_end_off - ctxt->filesize;
		if (file_off + dummy == ext_end_off) {
			ret = store_extent(ctxt, hashes, extent);
			if (ret)
				return ret;
		}
	}
	return 0;
}

/*
 * Try to fill the buffer with more data from the file
 * Unprocessed data could live in the buffer: in this case,
 * we avoid re-reading that data and, instead, move it at the beginning
 * of the buffer and (try to) fill whatever space is left.
 * Returns 1 on success, 0 when EOF is reached, negative int on error.
 */
static int fill_buffer(struct scan_ctxt *ctxt, struct buffer *buffer)
{
	ssize_t ret;

	/*
	 * The entire buffer could be ignored. Let's fast forward
	 * and mark the buffer as faked
	 */
	if (is_area_ignored(ctxt->fiemap, ctxt->off, buffer->size)
			&& ctxt->off + buffer->size <= ctxt->filesize) {
		memset(buffer->buf, 0, buffer->size);
		buffer->dl_len = buffer->size;
		buffer->dl_offset = 0;
		buffer->faked = true;

		if (ctxt->filesize <= ctxt->off + buffer->size)
			return 0; /* Simulate EOF */
		return 1;
	}

	/* Move leftovers back at the begining of the buffer */
	if (buffer->dl_len != 0)
		memmove(buffer->buf, buffer->buf + buffer->dl_offset, buffer->dl_len);
	buffer->dl_offset = 0;

	buffer->faked = false;

	ret = pread(ctxt->fd, buffer->buf + buffer->dl_len,
		buffer->size - buffer->dl_len, ctxt->off + buffer->dl_len);
	if (ret > 0)
		buffer->dl_len += ret;

	/* We must never overflow */
	assert(buffer->dl_offset + buffer->dl_len <= buffer->size);

	if (ret < 0)
		return ret;

	if (ret == 0 || ctxt->off + buffer->dl_len == ctxt->filesize) /* EOF */
		return 0;
	return buffer->dl_len;
}

static inline bool is_inlined(struct scan_ctxt *ctxt)
{
	struct fiemap_extent *extent;

	extent = get_extent(ctxt->fiemap, ctxt->filesize - 1, NULL);
	return extent && extent->fe_flags & FIEMAP_EXTENT_DATA_INLINE;
}

static void csum_whole_file(struct file_to_scan *file)
{
	int ret = 0;

	_cleanup_(free_hashes) struct hashes hashes = {0,};
	_cleanup_(free_scan_ctxt) struct scan_ctxt ctxt = {0,};
	unsigned char file_digest[DIGEST_LEN];

	/*
	 * All writes go through the single shared scan writer connection,
	 * serialized (and batched) behind the write lock.
	 */
	struct dbhandle *db = scan_writer;
	static __thread struct buffer buffer = {0,};

	/* Dummy variables used to trigger the cleanup code */
	_cleanup_(pscan_reset_thread) struct pscan_thread *tprogress = NULL;
	_cleanup_(freep) char *path = file->path;
	_cleanup_(freep) struct file_to_scan *clean_file = file;

	/* Used to detected eof if file changed since
	 * we stat() it
	 */
	bool eof_reached = false;

	/* Prevent close on fd 0 if, somehow, an error occurs before we open */
	ctxt.fd = -1;

	if (!(buffer.buf)) {
		ret = prepare_buffer(&buffer);
		if (ret) {
			eprintf("unable to prepare our read buffer\n");
			return;
		}
	} else {
		/* Clean leftovers from another call */
		buffer.dl_offset = 0;
		buffer.dl_len = 0;
	}

	if (!db) {
		eprintf("csum_whole_file: unable to connect to the database\n");
		return;
	}

	/* Claimed per file, not per thread: see pscan_claim_slot(). */
	tprogress = pscan_claim_slot(gettid(), thread_scanning);
	abort_on(!tprogress);

	tprogress->file_scanned_bytes = 0;
	tprogress->file_total_bytes = file->filesize;
	strncpy(tprogress->file_path, file->path, PATH_MAX);

	ctxt.filesize = file->filesize;
	ctxt.file_csum = start_running_checksum();
	if (!ctxt.file_csum)
		return;

	ctxt.fd = open(file->path, O_RDONLY);
	if (ctxt.fd == -1) {
		eprintf("csum_whole_file: Error %d: %s while opening file \"%s\". "
			"Skipping.\n", errno, strerror(errno), file->path);
		return;
	}

	/* We read each file once, front to back: ask for aggressive readahead. */
	posix_fadvise(ctxt.fd, 0, 0, POSIX_FADV_SEQUENTIAL);

	ctxt.fiemap = do_fiemap(ctxt.fd);
	if (!ctxt.fiemap)
		return;

	if (!allocate_hashes(&hashes, &ctxt)) {
		eprintf("allocate_hashes failed\n");
		return;
	}

	/*
	 * Main loop:
	 * - grab some data into the buffer
	 * - try to process as must entire blocks as possible
	 * - consume that amount of bytes for the file csum
	 * - consume that amount of bytes for the extents
	 * loop again until pread returns 0 or
	 * until we reach the expected EOF, based on the expected filesize
	 */
	while (ctxt.off < ctxt.filesize) {
		/* In the buffer, how much bytes are processed as blocks
		 * Extents processing and file processing will not consumme
		 * more than that amount of bytes
		 */
		ssize_t bytes_processed = 0;

		ret = fill_buffer(&ctxt, &buffer);
		if (ret < 0) {
			ret = errno;
			eprintf("Unable to read file %s: %s\n",
				file->path, strerror(ret));
			return;
		}

		if (ret == 0)
			eof_reached = true;

		bytes_processed = process_blocks(&ctxt, &buffer, &hashes);
		if (bytes_processed < 0) {
			eprintf("process_blocks failed somehow\n");
			return;
		}

		tprogress->file_scanned_bytes += bytes_processed;
		tprogress->total_scanned_bytes += bytes_processed;

		/* Process the last partial block */
		if (eof_reached && (size_t)bytes_processed < buffer.dl_len) {
			ret = process_block(buffer.buf + bytes_processed,
					    buffer.dl_len - bytes_processed,
					    ctxt.off + bytes_processed,
					    &hashes);
			if (ret) {
				eprintf("Unable to process %s's last block\n", file->path);
				return;
			}

			bytes_processed += buffer.dl_len - bytes_processed;
		}

		add_to_running_checksum(ctxt.file_csum, (unsigned char*)(buffer.buf), bytes_processed);

		if (!options.only_whole_files) {
			ret = process_extents(&ctxt, &buffer, &hashes, bytes_processed);
			if (ret)
				break;
		}

		buffer.dl_offset = bytes_processed;
		buffer.dl_len -= bytes_processed;

		/* Ack the processed data and move the current offset accordingly */
		ctxt.off += bytes_processed;

		if (eof_reached)
			/* File may have change */
			break;
	}

	if (ctxt.off != ctxt.filesize) {
		eprintf("file %s changed\n", file->path);
		return;
	}

	finish_running_checksum(ctxt.file_csum, file_digest);
	ctxt.file_csum = NULL;

	/*
	 * We've read the whole file once and won't touch it again this scan.
	 * Drop it from the page cache so hashing a large tree doesn't evict
	 * everything else and push page allocation into the reclaim slowpath.
	 */
	posix_fadvise(ctxt.fd, 0, 0, POSIX_FADV_DONTNEED);

	/*
	 * Whether the last extent is inlined is a pure fiemap scan; compute it
	 * once here rather than twice under the write lock below.
	 */
	bool inlined = is_inlined(&ctxt);

	tprogress->status = thread_waiting_lock;
	dbfile_lock();
	tprogress->status = thread_committing;
	ret = scan_write_begin();
	if (ret) {
		dbfile_unlock();
		return;
	}

	/* Do not store the blocks if the file is inlined */
	if (hashes.blocks_index != 0 && !inlined) {
		ret = dbfile_store_block_hashes(db, file->fileid,
						hashes.blocks_index, hashes.blocks);
		if (ret) {
			scan_write_abort();
			dbfile_unlock();
			return;
		}
	}


	if (hashes.extents_index != 0) {
		ret = dbfile_store_extent_hashes(db, file->fileid, hashes.extents_index, hashes.extents);
		if (ret) {
			scan_write_abort();
			dbfile_unlock();
			return;
		}
	}

	/* Flag the file if its last extent is INLINED.
	 * Attempt to deduplicate those will never succeed and will produce a lot
	 * of needless work: https://github.com/markfasheh/duperemove/issues/316
	 */
	ret = dbfile_update_scanned_file(db, file->fileid, file_digest,
			inlined ? FILE_INLINED : 0);
	if (ret) {
		scan_write_abort();
		dbfile_unlock();
		return;
	}

	ret = scan_write_end();
	if (ret) {
		dbfile_unlock();
		return;
	}

	dbfile_unlock();
}

int add_exclude_pattern(const char *pattern)
{
	char cwd[PATH_MAX] = { 0, };

	/* Overallocate to peace the compiler. */
	char exp_pattern[PATH_MAX * 2 + 1] = { 0, };
	struct exclude_file *exclude = malloc(sizeof(*exclude));

	if (!exclude)
		return 1;

	if (pattern[0] == '/') {
		exclude->pattern = strdup(pattern);
	} else {
		getcwd(cwd, PATH_MAX);

		if (strlen(cwd) + strlen(pattern) > PATH_MAX) {
			eprintf("Error: cannot prepend cwd to %s\n", pattern);
			return 1;
		}

		sprintf(exp_pattern, "%s/%s", cwd, pattern);
		exclude->pattern = strdup(exp_pattern);
	}

	exclude->is_glob = strpbrk(exclude->pattern, "*?[\\") != NULL;

	vprintf("Adding exclude pattern: %s\n", exclude->pattern);

	SLIST_INSERT_HEAD(&exclude_head, exclude, list);
	return 0;
}

/*
 * Set of inodes, keyed by (ino, subvol), that this scan has already written a
 * filerec for.
 *
 * The scan batches many files into a single uncommitted transaction on the
 * shared writer connection. The change-detection lookup in __scan_file() runs
 * on a separate read connection, which under WAL cannot see rows the writer has
 * not committed yet. So when two hardlinks to the same inode are visited within
 * one batch, the second lookup misses the first's pending row and we would
 * INSERT OR REPLACE the same (ino, subvol) again. That REPLACE deletes the
 * pending row - cascade-deleting the hashes a worker is still writing for it -
 * and the resulting constraint failure aborts the whole batch, silently losing
 * every file in it.
 *
 * duperemove keeps exactly one filerec per inode anyway (UNIQUE(ino, subvol)),
 * so track the inodes written this scan and skip any further hardlink to one we
 * have already handled. Touched only from the single __scan_file() consumer
 * (not the walker threads), so it needs no locking.
 */
static GHashTable *seen_inodes;

struct ino_key {
	uint64_t	ino;
	uint64_t	subvol;
};

static guint ino_key_hash(gconstpointer p)
{
	const struct ino_key *k = p;

	return g_int64_hash(&k->ino) ^ g_int64_hash(&k->subvol);
}

static gboolean ino_key_equal(gconstpointer a, gconstpointer b)
{
	const struct ino_key *ka = a, *kb = b;

	return ka->ino == kb->ino && ka->subvol == kb->subvol;
}

static bool seen_inode(uint64_t ino, uint64_t subvol)
{
	struct ino_key k = { .ino = ino, .subvol = subvol };

	return seen_inodes && g_hash_table_contains(seen_inodes, &k);
}

static void mark_inode_seen(uint64_t ino, uint64_t subvol)
{
	struct ino_key *k;

	if (!seen_inodes)
		return;

	k = malloc(sizeof(*k));
	if (!k)	/* On OOM just skip; the worst case is the pre-fix behavior. */
		return;

	k->ino = ino;
	k->subvol = subvol;
	g_hash_table_add(seen_inodes, k);
}

void filescan_init(void)
{
	abort_on(scan_pool.pool);
	abort_on(scan_writer_open());
	seen_inodes = g_hash_table_new_full(ino_key_hash, ino_key_equal,
					    free, NULL);
	abort_on(!seen_inodes);
	setup_pool(&scan_pool, csum_whole_file, NULL, options.io_threads);
	abort_on(!scan_pool.pool);
}

void filescan_free(void)
{
	free_pool(&scan_pool);
	/* All workers have joined: flush the batched reads and drop the writer. */
	scan_read_flush();
	scan_writer_close();
	subvol_cache_free();
	verified_dev_free();
	if (seen_inodes) {
		g_hash_table_destroy(seen_inodes);
		seen_inodes = NULL;
	}
}

void add_file_fdupes(char *path)
{
	struct statx st;
	int ret;

	ret = statx(AT_FDCWD, path, 0, STATX_BASIC_STATS, &st);
	if (ret || !(st.stx_mask & STATX_BASIC_STATS)) {
		eprintf("statx on %s: %s\n", path, strerror(errno));
		return;
	}

	/*
	 * Filerecs are keyed by inode. Two hardlinks in the same fdupes group
	 * would produce a duplicate key and abort in insert_filerec(). They
	 * already share storage, so deduping them is pointless anyway - skip
	 * any inode we've already listed.
	 */
	if (filerec_find(st.stx_ino)) {
		vprintf("Skipping %s: hardlink to an already-listed file\n",
			path);
		return;
	}
	filerec_new(path, st.stx_ino, st.stx_size);
}
