#ifndef	__DBFILE_H__
#define	__DBFILE_H__

#include <stdlib.h>
#include <stdint.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <sys/types.h>
#include <uuid/uuid.h>

#include "util.h"
#include "csum.h"
#include "threads.h"

struct filerec;
struct block_csum;
struct extent_csum;
struct results_tree;
struct dbfile_config;

/*
 * oans's own hashfile format generation. Forked from upstream duperemove's 4.1;
 * bumped to 5.0 as a deliberate clean break so oans and duperemove never share
 * a version number.
 */
#define DB_FILE_MAJOR	5
#define DB_FILE_MINOR	0

/*
 * SQLite application_id stamped into every oans hashfile ("oans" in ASCII).
 * oans requires this brand: dbfile_check() refuses any hashfile that does not
 * carry it (a foreign file, or a pre-brand/duperemove one). A brand-new empty
 * hashfile is stamped before the check; existing files must already carry it.
 */
#define OANS_APP_ID	0x6f616e73

struct stmts {
	sqlite3_stmt *insert_block;
	sqlite3_stmt *insert_extent;
	sqlite3_stmt *update_scanned_file;
	sqlite3_stmt *update_extent_poff;
	sqlite3_stmt *write_file;
	sqlite3_stmt *remove_block_hashes;
	sqlite3_stmt *remove_extent_hashes;
	sqlite3_stmt *load_filerec;
	sqlite3_stmt *get_duplicate_blocks;
	sqlite3_stmt *get_duplicate_extents;
	sqlite3_stmt *get_duplicate_files;
	sqlite3_stmt *get_nondupe_extents;
	sqlite3_stmt *delete_file;
	sqlite3_stmt *delete_file_by_id;
	sqlite3_stmt *select_file_changes;
	sqlite3_stmt *count_b_hashes;
	sqlite3_stmt *count_e_hashes;
	sqlite3_stmt *count_files;
	sqlite3_stmt *get_max_dedupe_seq;
	sqlite3_stmt *delete_unscanned_files;
	sqlite3_stmt *rename_file;
};

struct dbhandle {
	sqlite3 *db;
	struct stmts stmts;
};

struct file {
	int64_t		id;
	char		filename[PATH_MAX + 1];
	uint64_t	ino;
	uint64_t	subvol;
	size_t		size;
	uint64_t	blocks;
	uint64_t	mtime;
	unsigned int	dedupe_seq;
	char		digest[DIGEST_LEN];
	unsigned int	flags;
};

struct dbhandle *dbfile_open_handle(char *filename);
/* Read-only open for report modes: no writes, safe to run while another oans
 * is deduping the same hashfile. NULL if missing or not an oans hashfile. */
struct dbhandle *dbfile_open_handle_ro(char *filename);
void dbfile_close_handle(struct dbhandle *db);

/*
 * Per-connection SQLite page-cache budgets, in KiB. Every connection defaults
 * to DB_CACHE_KB_DEFAULT; on a large hashfile that cache fills toward the cap,
 * so connections that don't run the heavy dedupe joins are given a smaller
 * budget to bound peak RSS (the search pool alone opens up to cpu_threads of
 * them). Override a handle's budget with dbfile_set_cache_kb() after opening.
 */
#define DB_CACHE_KB_DEFAULT	65536	/* loader / reader / writer: the heavy joins */
#define DB_CACHE_KB_SEARCH	32768	/* find_dupes search pool (x cpu_threads) */
#define DB_CACHE_KB_WALKER	 2048	/* walkers only read the fs-uuid config, if that */

void dbfile_set_cache_kb(struct dbhandle *db, unsigned int kb);

/*
 * Open the database (options.hashfile)
 * On success, the handle is registered to be freed when the thread pool is freed
 */
struct dbhandle *dbfile_open_handle_thread(struct threads_pool *pool);

void dbfile_lock(void);
void dbfile_unlock(void);

/* Config-table key holding a persisted --autotune result (io-threads). */
#define AUTOTUNE_CONFIG_KEY	"autotune_io_threads"

struct dbfile_config {
	unsigned int	blocksize;
	int		major;
	int		minor;
	char		hash_type[8];
	unsigned int	dedupe_seq;
	uuid_t		fs_uuid;
	/* Persisted --autotune winner, 0 if never tuned (see AUTOTUNE_CONFIG_KEY). */
	unsigned int	autotune_io_threads;
};
int dbfile_get_config(sqlite3 *db, struct dbfile_config *cfg);
int __dbfile_sync_config(sqlite3 *db, struct dbfile_config *cfg);
int dbfile_sync_config(struct dbhandle *db, struct dbfile_config *cfg);

/*
 * Store a single integer under `key` in the config key/value table (used to
 * persist the --autotune result). Returns 0 on success. The value is read back
 * as part of struct dbfile_config (see dbfile_get_config).
 */
int dbfile_set_config_int(struct dbhandle *db, const char *key, int64_t val);

/*
 * Self-describing hashfile: the scan-shaping options plus the roots and
 * user exclude patterns of a run, so `oans --hashfile=X` (no file arguments)
 * can replay the last run. Ephemeral knobs (threads, verbosity, colour,
 * batchsize) are deliberately not stored; blocksize/hash live in dbfile_config
 * already. roots/excludes are heap arrays owned by the struct.
 */
struct scan_config {
	int		run_dedupe;
	int		recurse;
	int		skip_zeroes;
	int		only_whole_files;
	int		do_block_hash;
	int		dedupe_same_file;
	uint64_t	min_filesize;
	char		**roots;
	int		nroots;
	char		**excludes;
	int		nexcludes;
};

/* Persist sc (options + roots + excludes), replacing any previously stored set. */
int dbfile_store_scan_config(struct dbhandle *db, const struct scan_config *sc);
/*
 * Load the stored scan config into sc (zeroed first). Returns 1 if a stored
 * configuration was present, 0 if none, <0 on error. Caller frees with
 * scan_config_free().
 */
int dbfile_load_scan_config(struct dbhandle *db, struct scan_config *sc);
void scan_config_free(struct scan_config *sc);

/*
 * One recorded run, appended to the hashfile's run_history at the end of each
 * invocation (see dbfile_record_run). Fed to --history and the --json export.
 */
struct run_record {
	int64_t		ts;		/* unix seconds */
	int64_t		duration_ms;
	uint64_t	files_scanned;
	uint64_t	reclaimed;	/* bytes: space reclaimed (kernel-deduped) */
	uint64_t	groups;
	int		deduped;	/* 1 if -d, 0 for a scan-only run */
};
int dbfile_record_run(struct dbhandle *db, const struct run_record *r);

/* Lifetime totals over run_history, for --history and --json. */
struct run_summary {
	uint64_t	runs;
	uint64_t	total_reclaimed;
	uint64_t	total_files;
	int64_t		first_ts;
	int64_t		last_ts;
};
int dbfile_get_run_summary(struct dbhandle *db, struct run_summary *s);

struct dbfile_stats {
	uint64_t	num_b_hashes;
	uint64_t	num_e_hashes;
	uint64_t	num_files;
};
int dbfile_get_stats(struct dbhandle *db, struct dbfile_stats *stats);

/* Run a query returning one integer and return its value; 0 on error/no row. */
uint64_t dbfile_query_u64(sqlite3 *db, const char *sql);

/* VACUUM the hashfile, but only when enough of it is free to be worth it. */
void dbfile_maybe_vacuum(struct dbhandle *db);

struct hash_tree;

/*
 * Load hashes into hash_tree only if they have a duplicate in the db.
 * The extent search is later run on the resulting hash_tree.
 *
 * All three loaders process one dedupe pass: the groups with at least one
 * member whose generation is in (seq_lo, seq_hi], plus their partners from
 * any generation <= seq_hi.
 */
int dbfile_load_block_hashes(struct dbhandle *db, struct hash_tree *hash_tree,
			     unsigned int seq_lo, unsigned int seq_hi);
int dbfile_load_extent_hashes(struct dbhandle *db, struct results_tree *res,
			      unsigned int seq_lo, unsigned int seq_hi);

struct file_extent {
	uint64_t	poff;
	uint64_t	loff;
	uint64_t	len;
};
int dbfile_load_nondupe_file_extents(struct dbhandle *db, struct filerec *file,
				     struct file_extent **ret_extents,
				     unsigned int *num_extents);

int dbfile_load_one_filerec(struct dbhandle *db, int64_t fileid,
				struct filerec **file);

/*
 * Following are used during file scan stage to get our hashes into
 * the database.
 */
struct dbhandle *dbfile_get_handle(void);

int64_t dbfile_store_file_info(struct dbhandle *db, struct file *dbfile);
int dbfile_store_block_hashes(struct dbhandle *db, int64_t fileid,
				uint64_t nb_hash, struct block_csum *hashes);
int dbfile_store_extent_hashes(struct dbhandle *db, int64_t fileid,
				uint64_t nb_hash, struct extent_csum *hashes);
int dbfile_update_scanned_file(struct dbhandle *db, int64_t fileid,
				unsigned char *digest, unsigned int flags);
int dbfile_begin_trans(sqlite3 *db);
int dbfile_commit_trans(sqlite3 *db);
int dbfile_abort_trans(sqlite3 *db);
int dbfile_update_extent_poff(struct dbhandle *db, int64_t fileid,
				uint64_t loff, uint64_t poff);

/*
 * This is used for printing so we can get away with chars from sqlite
 * for now.
 */
typedef void (*iter_files_func)(char *filename, char *ino, char *subvol);
int dbfile_iter_files(struct dbhandle *db, iter_files_func func);

int dbfile_remove_extent_hashes(struct dbhandle *db, int64_t fileid);
int dbfile_remove_file(struct dbhandle *db, const char *filename);

int dbfile_describe_file(struct dbhandle *db, uint64_t ino, uint64_t subvol,
				struct file *dbfile);
int dbfile_load_same_files(struct dbhandle *db, struct results_tree *res,
			   unsigned int seq_lo, unsigned int seq_hi);

int dbfile_rename_file(struct dbhandle *db, int64_t fileid, char *path);

static inline void sqlite3_stmt_cleanup(void *p)
{
	sqlite3_finalize(*(sqlite3_stmt**) p);
}

static inline void sqlite3_close_cleanup(struct dbhandle **db)
{
	dbfile_close_handle(*db);
}

static inline void sqlite3_reset_stmt(sqlite3_stmt **stmt)
{
	sqlite3_reset(*stmt);
}

void dbfile_set_gdb(struct dbhandle *db);
int dbfile_remove_hashes(struct dbhandle *db, int64_t fileid);

unsigned int get_max_dedupe_seq(struct dbhandle *db);

/*
 * Approximate number of duplicate groups the dedupe phase will process, for a
 * progress total/ETA. Counts identical-file groups plus, unless whole_file_only,
 * duplicate-extent groups across the whole hashfile.
 */
uint64_t dbfile_count_dupe_groups(struct dbhandle *db, bool whole_file_only);
int dbfile_prune_unscanned_files(struct dbhandle *db);
int64_t dbfile_prune_missing_files(struct dbhandle *db, bool (*seen)(int64_t));

/* Build the find-dupes-phase indexes (deferred past the scan). See dbfile.c. */
int dbfile_create_search_indexes(struct dbhandle *db);
#endif	/* __DBFILE_H__ */
