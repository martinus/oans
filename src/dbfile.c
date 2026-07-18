#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sqlite3.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>

#include "csum.h"
#include "filerec.h"
#include "hash-tree.h"
#include "results-tree.h"
#include "file_scan.h"
#include "debug.h"

#include "dbfile.h"
#include "opt.h"

static struct dbhandle *gdb = NULL;

static sqlite3 *__dbfile_open_handle(char *filename, bool force_create,
				     bool readonly);

static GMutex io_mutex; /* Locks db writes */

#if (SQLITE_VERSION_NUMBER < 3007015)
#define	perror_sqlite(_err, _why)					\
	eprintf("%s(): Database error %d while %s: %s\n",	\
		__FUNCTION__, _err, _why, "[sqlite3_errstr() unavailable]")
#else
#define	perror_sqlite(_err, _why)					\
	eprintf("%s()/%ld: Database error %d while %s: %s\n",	\
		__FUNCTION__, syscall(SYS_gettid), _err, _why, sqlite3_errstr(_err))
#endif

/*
 * Explain why a hashfile could not be opened. SQLite collapses most failures
 * into a generic "unable to open database file"; the usual real cause is that
 * the parent directory does not exist (SQLite creates the file, not the dir),
 * so check for that and give an actionable hint.
 */
static void report_db_open_error(const char *filename, sqlite3 *db)
{
	char dir[PATH_MAX + 1];
	const char *slash = strrchr(filename, '/');
	struct stat st;

	if (slash == filename)
		snprintf(dir, sizeof(dir), "/");
	else if (slash)
		snprintf(dir, sizeof(dir), "%.*s", (int)(slash - filename), filename);
	else
		snprintf(dir, sizeof(dir), ".");

	if (stat(dir, &st) != 0 && errno == ENOENT) {
		eprintf("Error: cannot open hashfile \"%s\": directory \"%s\" "
			"does not exist.\n", filename, dir);
		if (filename[0] == '~')
			eprintf("       A leading '~' after --hashfile= is not "
				"expanded by the shell; use $HOME or a full path.\n");
		else
			eprintf("       Create it first, e.g.: mkdir -p \"%s\"\n",
				dir);
		return;
	}
	if (stat(dir, &st) == 0 && !S_ISDIR(st.st_mode)) {
		eprintf("Error: cannot open hashfile \"%s\": \"%s\" is not a "
			"directory.\n", filename, dir);
		return;
	}
	eprintf("Error opening hashfile \"%s\": %s\n", filename,
		sqlite3_errmsg(db));
}

struct dbhandle *dbfile_get_handle(void)
{
	return gdb;
}

void dbfile_lock(void)
{
	g_mutex_lock(&io_mutex);
}

void dbfile_unlock(void)
{
	g_mutex_unlock(&io_mutex);
}

static void dbfile_config_defaults(struct dbfile_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->blocksize = blocksize;
	memcpy(cfg->hash_type, HASH_TYPE, 8);

	cfg->major = DB_FILE_MAJOR;
	cfg->minor = DB_FILE_MINOR;
}

static int dbfile_get_dbpath(sqlite3 *db, char *path)
{
	int ret;
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;
	const char *buf;

#define GET_DBPATH "select file from pragma_database_list where name = 'main' limit 1;"
	ret = sqlite3_prepare_v2(db, GET_DBPATH, -1, &stmt, NULL);
	if (ret) {
		perror_sqlite(ret, "preparing statement");
		return ret;
	}

	ret = sqlite3_step(stmt);
	if (ret != SQLITE_ROW) {
		perror_sqlite(ret, "fetching database's backend path");
		return ret;
	}

	buf = (char *)sqlite3_column_text(stmt, 0);
	if (strnlen(buf, PATH_MAX) != 0) {
		strncpy(path, buf, PATH_MAX);
	} else {
		strcpy(path, "(null)");
	}

	return 0;
}

/*
 * Run a query returning one integer (a count, a PRAGMA, ...) and return its
 * value; 0 on error or no row.
 */
uint64_t dbfile_query_u64(sqlite3 *db, const char *sql)
{
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;
	uint64_t v = 0;

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
	    sqlite3_step(stmt) == SQLITE_ROW)
		v = sqlite3_column_int64(stmt, 0);
	return v;
}

/* Best-effort: a read-only handle can't write the header, which is fine. */
static void dbfile_stamp_application_id(sqlite3 *db)
{
	char sql[64];

	snprintf(sql, sizeof(sql), "PRAGMA application_id = %d;", OANS_APP_ID);
	sqlite3_exec(db, sql, NULL, NULL, NULL);
}

/* True for a brand-new hashfile: create_tables() has run but nothing has
 * written the config rows yet (that happens after the check, in sync_config). */
static bool dbfile_config_empty(sqlite3 *db)
{
	return dbfile_query_u64(db, "select 1 from config limit 1;") == 0;
}

static int dbfile_check(sqlite3 *db, struct dbfile_config *cfg)
{
	char path[PATH_MAX + 1];
	int app_id = 0;

	dbfile_get_dbpath(db, path);

	/*
	 * oans requires its brand. A brand-new file was stamped before this
	 * check (see dbfile_prepare); anything reaching here without the brand
	 * is a foreign or pre-brand (e.g. duperemove) hashfile - refuse it, and
	 * the caller recreates it as an oans file.
	 */
	app_id = (int)dbfile_query_u64(db, "PRAGMA application_id;");
	if (app_id != OANS_APP_ID) {
		eprintf("Hashfile %s is not an oans hashfile "
			"(application_id 0x%08x); refusing to use it\n", path,
			(unsigned)app_id);
		return EIO;
	}

	if (cfg->major != DB_FILE_MAJOR || cfg->minor != DB_FILE_MINOR) {
		eprintf("Hash db version mismatch (mine: %d.%d, file: %d.%d)\n",
			DB_FILE_MAJOR, DB_FILE_MINOR, cfg->major, cfg->minor);
		return EIO;
	}


	if (strncasecmp(cfg->hash_type, HASH_TYPE, 8)) {
		eprintf("Error: Hashfile %s uses \"%.*s\" for checksums "
			"but we are using %.*s.\nYou are probably "
			"using a hashfile generated from an old version, "
			"which cannot be read anymore.\n", path, 8,
			cfg->hash_type, 8, HASH_TYPE);
		return EINVAL;
	}

	if (cfg->blocksize != blocksize) {
		vprintf("Using blocksize %uK from hashfile (%uK "
			"blocksize requested).\n", cfg->blocksize/1024,
			blocksize/1024);
		blocksize = cfg->blocksize;
	}

	return 0;
}

static int create_tables(sqlite3 *db)
{
	int ret;

#define CREATE_TABLE_CONFIG						\
"CREATE TABLE IF NOT EXISTS config(keyname TEXT PRIMARY KEY NOT NULL, "	\
"keyval BLOB, UNIQUE(keyname));"
	ret = sqlite3_exec(db, CREATE_TABLE_CONFIG, NULL, NULL, NULL);
	if (ret)
		goto out;

/*
 * path_hash is csum_path(filename): a compact 64-bit stand-in for the full
 * path in the uniqueness index. The path text is still stored (files must be
 * opened by name to dedupe), but UNIQUE is enforced on the hash so its
 * automatic index costs 8 bytes/row instead of a second copy of every path.
 */
#define	CREATE_TABLE_FILES						\
"CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY NOT NULL, "	\
"filename TEXT NOT NULL, path_hash INTEGER NOT NULL, "			\
"ino INTEGER, subvol INTEGER, size INTEGER, "				\
"mtime INTEGER, dedupe_seq INTEGER, digest BLOB, "			\
"flags INTEGER, UNIQUE(ino, subvol), UNIQUE(path_hash));"
	ret = sqlite3_exec(db, CREATE_TABLE_FILES, NULL, NULL, NULL);
	if (ret)
		goto out;

#define	CREATE_TABLE_EXTENTS						\
"CREATE TABLE IF NOT EXISTS extents(digest BLOB KEY NOT NULL, "		\
"fileid INTEGER, loff INTEGER, poff INTEGER, len INTEGER, "		\
"UNIQUE(fileid, loff, len) "						\
"FOREIGN KEY(fileid) REFERENCES files(id) ON DELETE CASCADE);"
	ret = sqlite3_exec(db, CREATE_TABLE_EXTENTS, NULL, NULL, NULL);
	if (ret)
		goto out;

#define	CREATE_TABLE_BLOCKS						\
"CREATE TABLE IF NOT EXISTS blocks(digest BLOB KEY NOT NULL, "		\
"fileid INTEGER, loff INTEGER, "					\
"UNIQUE(fileid, loff) "							\
"FOREIGN KEY(fileid) REFERENCES files(id) ON DELETE CASCADE);"
	ret = sqlite3_exec(db, CREATE_TABLE_BLOCKS, NULL, NULL, NULL);
	if (ret)
		goto out;

	/*
	 * Self-describing hashfile: the roots and exclude patterns of the last
	 * run (see dbfile_store_scan_config). One column, ordered by insertion
	 * rowid so replay preserves argument order.
	 */
#define CREATE_TABLE_SCAN_ROOTS						\
"CREATE TABLE IF NOT EXISTS scan_roots(path TEXT NOT NULL);"
	ret = sqlite3_exec(db, CREATE_TABLE_SCAN_ROOTS, NULL, NULL, NULL);
	if (ret)
		goto out;

#define CREATE_TABLE_SCAN_EXCLUDES					\
"CREATE TABLE IF NOT EXISTS scan_excludes(pattern TEXT NOT NULL);"
	ret = sqlite3_exec(db, CREATE_TABLE_SCAN_EXCLUDES, NULL, NULL, NULL);
	if (ret)
		goto out;

	/*
	 * One row per oans run (see dbfile_record_run): a timeline of what each
	 * run reclaimed, for `--history` and the `--json` metrics export.
	 */
#define CREATE_TABLE_RUN_HISTORY					\
"CREATE TABLE IF NOT EXISTS run_history("				\
"ts INTEGER NOT NULL, duration_ms INTEGER NOT NULL, "			\
"files_scanned INTEGER NOT NULL, reclaimed INTEGER NOT NULL, "		\
"groups INTEGER NOT NULL, kernel_bytes INTEGER NOT NULL, "		\
"deduped INTEGER NOT NULL);"
	ret = sqlite3_exec(db, CREATE_TABLE_RUN_HISTORY, NULL, NULL, NULL);

out:
	if (ret)
		perror_sqlite(ret, "creating database tables");

	return ret;
}

static int create_indexes(sqlite3 *db)
{
	int ret;

	/*
	 * Drop indexes that only duplicate the leftmost prefix of a UNIQUE
	 * constraint's automatic index, so sqlite never picks them: ino_subvol
	 * duplicates UNIQUE(ino, subvol); extents/blocks fileid duplicate
	 * UNIQUE(fileid, loff[, len]). They cost space in the hashfile and an
	 * extra index update on every insert for no query benefit. Drop them so
	 * hashfiles written by older versions shed them too (space is reclaimed
	 * on the next vacuum).
	 */
#define DROP_REDUNDANT_INDEXES						\
"drop index if exists idx_files_ino_subvol;"				\
"drop index if exists idx_extents_fileid;"				\
"drop index if exists idx_blocks_fileid;"
	ret = sqlite3_exec(db, DROP_REDUNDANT_INDEXES, NULL, NULL, NULL);
	if (ret)
		goto out;

	/*
	 * dedupe_seq is low-cardinality and consulted before/around scanning, so
	 * keep it maintained from the start. The digest indexes, by contrast, are
	 * only used by the find-dupes phase - they are built after the scan, see
	 * dbfile_create_search_indexes().
	 */
#define CREATE_FILES_DEDUPESEQ_INDEX					\
"create index if not exists idx_files_dedupeseq on files(dedupe_seq);"
	ret = sqlite3_exec(db, CREATE_FILES_DEDUPESEQ_INDEX, NULL, NULL, NULL);
	if (ret)
		goto out;

out:
	if (ret)
		perror_sqlite(ret, "creating database index");
	return ret;
}

/*
 * Indexes used only by the find-dupes phase, not while scanning. They are
 * created after the scan's bulk insert rather than at open, so the first scan
 * of a fresh hashfile does not pay to maintain them on every insert - random
 * digest keys are the worst case for incremental B-tree maintenance, and a
 * one-shot build sorts once instead. Once built they persist in the hashfile,
 * so later incremental scans keep them up to date cheaply and the "if not
 * exists" below is a no-op.
 */
int dbfile_create_search_indexes(struct dbhandle *db)
{
	int ret;

#define CREATE_SEARCH_INDEXES						\
"create index if not exists idx_blocks_digest on blocks(digest);"	\
"create index if not exists idx_extents_digest_len on extents(digest, len);" \
"create index if not exists idx_files_digest_size on files(digest, size);"
	ret = sqlite3_exec(db->db, CREATE_SEARCH_INDEXES, NULL, NULL, NULL);
	if (ret)
		perror_sqlite(ret, "creating search indexes");
	return ret;
}

static int dbfile_set_modes(sqlite3 *db)
{
	int ret;

	ret = sqlite3_exec(db, "PRAGMA synchronous = OFF", NULL, NULL, NULL);
	if (ret) {
		perror_sqlite(ret, "configuring database (sync pragma)");
		return ret;
	}

	ret = sqlite3_exec(db, "PRAGMA journal_mode = WAL", NULL, NULL, NULL);
	if (ret) {
		perror_sqlite(ret, "configuring database (journal mode)");
		return ret;
	}

	ret = sqlite3_exec(db, "PRAGMA cache_size = -256000", NULL, NULL, NULL);
	if (ret) {
		perror_sqlite(ret, "configuring database (cache size)");
		return ret;
	}

	ret = sqlite3_exec(db, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);
	if (ret) {
		perror_sqlite(ret, "enabling foreign keys");
		return ret;
	}

	/*
	 * Wait out transient lock contention instead of failing immediately.
	 * The hashfile is touched by several connections (the listing reader,
	 * the batched writer, the csum and dedupe workers); without a timeout a
	 * brief overlap - e.g. a WAL checkpoint racing a write - surfaces as
	 * "database is locked" (SQLITE_BUSY). 30s comfortably covers the ~10s
	 * batched write transaction.
	 */
	ret = sqlite3_exec(db, "PRAGMA busy_timeout = 30000", NULL, NULL, NULL);
	if (ret) {
		perror_sqlite(ret, "setting busy timeout");
		return ret;
	}

	/*
	 * The default (no --hashfile) database is an in-memory shared-cache db
	 * (see MEMDB_FILENAME) shared by the listing reader, the batched writer
	 * and the csum workers - all separate connections. Shared-cache does
	 * table-level locking between connections, so the reader's lock on the
	 * files table makes the writer's INSERT fail with SQLITE_LOCKED ("table
	 * is locked"). read_uncommitted lets readers proceed without that lock;
	 * it only affects shared-cache mode and is a no-op for WAL file dbs.
	 */
	ret = sqlite3_exec(db, "PRAGMA read_uncommitted = 1", NULL, NULL, NULL);
	if (ret) {
		perror_sqlite(ret, "enabling read-uncommitted");
		return ret;
	}

	return ret;
}

/*
 * Set when this run built the hashfile from scratch - a brand-new file or one
 * recreated after a failed check. Such a file is written at insert density with
 * no freelist, so dbfile_maybe_vacuum() forces a one-off compaction for it.
 */
static bool hashfile_rebuilt;

static int dbfile_prepare(sqlite3 *db, bool readonly)
{
	struct dbfile_config cfg;
	int ret;
	char dbpath[PATH_MAX + 1];

	/*
	 * Read-only open (report modes: --stats/--history/--json/-L): verify the
	 * file is an oans hashfile but issue no writes at all - no table/index
	 * DDL, no config sync, no recreate-on-mismatch. This is what makes those
	 * commands safe to run while another oans is deduping: they never contend
	 * for the WAL write lock (and never clobber a foreign file).
	 */
	if (readonly) {
		if (dbfile_get_config(db, &cfg))
			return -1;
		return dbfile_check(db, &cfg);
	}

	ret = create_tables(db);
	if (ret) {
		perror_sqlite(ret, "creating tables");
		return ret;
	}

	ret = create_indexes(db);
	if (ret) {
		perror_sqlite(ret, "creating indexes");
		return ret;
	}

	ret = dbfile_get_dbpath(db, dbpath);
	if (ret)
		return ret;

	if (strcmp("(null)", dbpath) != 0) {
		ret = chmod(dbpath, S_IRUSR|S_IWUSR);
		if (ret) {
			perror("setting db file permissions");
			return ret;
		}
	}

	/*
	 * A brand-new hashfile (empty config) is ours: brand it now, before the
	 * strict application_id check below. An existing file must already carry
	 * the brand or dbfile_check() rejects it and we recreate it fresh.
	 */
	if (dbfile_config_empty(db)) {
		dbfile_stamp_application_id(db);
		hashfile_rebuilt = true;
	}

	ret = dbfile_get_config(db, &cfg);
	if (ret) {
		perror_sqlite(ret, "reading initial db config");
		return ret;
	}

	ret = dbfile_check(db, &cfg);
	if (ret && strcmp("(null)", dbpath) != 0) {
		eprintf("Recreating hashfile ..\n");
		sqlite3_close(db);
		ret = unlink(dbpath);
		if ( ret && errno != ENOENT) {
			ret = errno;
			eprintf("Error %d while unlinking old "
				"db file \"%s\" : %s\n", ret, dbpath,
				strerror(ret));
			return ret;
		}

		db = __dbfile_open_handle(dbpath, false, false);
		return dbfile_prepare(db, false);
	}

	/* May store the default config, if fields were missing
	 * or if the database did not exist
	 */
	ret = __dbfile_sync_config(db, &cfg);
	if (ret) {
		perror_sqlite(ret, "sync db config");
		return ret;
	}

	return 0;
}


#define MEMDB_FILENAME		"file::memory:?cache=shared"
#define OPEN_FLAGS		(SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX|SQLITE_OPEN_URI)
#define OPEN_FLAGS_CREATE	(OPEN_FLAGS|SQLITE_OPEN_CREATE)
static sqlite3 *__dbfile_open_handle(char *filename, bool force_create,
				     bool readonly)
{
	int ret;
	sqlite3 *db;

	if (!filename) {
		filename = MEMDB_FILENAME;
		force_create = true;
	}

	if (force_create)
		ret = sqlite3_open_v2(filename, &db, OPEN_FLAGS_CREATE, NULL);
	else
		ret = sqlite3_open_v2(filename, &db, OPEN_FLAGS, NULL);

	/* A read-only report must not conjure a hashfile that isn't there. */
	if (ret == SQLITE_CANTOPEN && !force_create && !readonly) {
		vprintf("Cannot open an existing hashfile, retrying in create mode\n");
		sqlite3_close(db);
		return __dbfile_open_handle(filename, true, false);
	}

	if (ret) {
		report_db_open_error(filename, db);
		sqlite3_close(db);
		return NULL;
	}

	ret = dbfile_set_modes(db);
	if (ret) {
		sqlite3_close(db);
		return NULL;
	}

	ret = dbfile_prepare(db, readonly);
	if (ret) {
		sqlite3_close(db);
		return NULL;
	}

	return db;
}

#define dbfile_prepare_stmt(member, query) do {							\
	int ret = sqlite3_prepare_v2(result->db, query, -1, &(result->stmts.member), NULL);	\
	if (ret) {										\
		perror_sqlite(ret, "preparing stmt");						\
		goto err;									\
	}											\
} while (0)

static struct dbhandle *open_handle(char *filename, bool readonly)
{
	struct dbhandle *result = calloc(1, sizeof(struct dbhandle));
	result->db = __dbfile_open_handle(filename, false, readonly);

	if (!result->db)
		goto err;

#define	INSERT_BLOCK							\
"INSERT INTO blocks (fileid, loff, digest) VALUES (?1, ?2, ?3);"
	dbfile_prepare_stmt(insert_block, INSERT_BLOCK);

#define	INSERT_EXTENTS							\
"INSERT INTO extents (fileid, loff, poff, len, digest) "		\
"VALUES (?1, ?2, ?3, ?4, ?5);"
	dbfile_prepare_stmt(insert_extent, INSERT_EXTENTS);

#define UPDATE_SCANNED_FILE						\
"UPDATE files SET digest = ?1, flags = ?2 where id = ?3;"
	dbfile_prepare_stmt(update_scanned_file, UPDATE_SCANNED_FILE);

#define	UPDATE_EXTENT_POFF						\
"update extents set poff = ?1 "						\
"where fileid = ?2 and loff = ?3;"
	dbfile_prepare_stmt(update_extent_poff, UPDATE_EXTENT_POFF);

#define	WRITE_FILE							\
"insert or replace into files (ino, subvol, filename, path_hash, size, "\
"mtime, dedupe_seq) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);"
	dbfile_prepare_stmt(write_file, WRITE_FILE);

#define REMOVE_BLOCK_HASHES						\
"delete from blocks where fileid = ?1;"
	dbfile_prepare_stmt(remove_block_hashes, REMOVE_BLOCK_HASHES);

#define REMOVE_EXTENT_HASHES						\
"delete from extents where fileid = ?1;"
	dbfile_prepare_stmt(remove_extent_hashes, REMOVE_EXTENT_HASHES);

#define LOAD_FILEREC							\
"select filename, size from files where id = ?1;"
	dbfile_prepare_stmt(load_filerec, LOAD_FILEREC);

/* Same generation-pass reduction as GET_DUPLICATE_EXTENTS, keyed on digest. */
#define GET_DUPLICATE_BLOCKS						\
"with grp(digest) as ( "						\
"	select blocks.digest from blocks "				\
"	join files on fileid = id "					\
"	where dedupe_seq <= ?2 and blocks.digest in ( "			\
"		select blocks.digest from blocks "			\
"		join files on fileid = id "				\
"		where dedupe_seq > ?1 and dedupe_seq <= ?2) "		\
"	group by blocks.digest having count(*) > 1) "			\
"select blocks.digest, fileid, loff from blocks "			\
"join files on fileid = id "						\
"where blocks.digest in (select digest from grp) and ( "		\
"	(dedupe_seq > ?1 and dedupe_seq <= ?2) "			\
"	or blocks.rowid in ( "						\
"		select min(b.rowid) from blocks b "			\
"		join files f on b.fileid = f.id "			\
"		where f.dedupe_seq <= ?1 "				\
"		and b.digest in (select digest from grp) "		\
"		group by b.digest)) "					\
"order by (dedupe_seq > ?1), fileid;"
	dbfile_prepare_stmt(get_duplicate_blocks, GET_DUPLICATE_BLOCKS);

/*
 * We need to select on both digest and len, otherwise we
 * could run into a situation where a single extent with a
 * colliding hash but different length gets placed into the
 * results tree, which will get very angry when it has a
 * result of only one extent.
 */
/*
 * Same generation-pass reduction as GET_DUPLICATE_FILES: load the extents new
 * this pass plus one already-deduped representative per group (min rowid among
 * members from an earlier pass), rather than every member. Extent dedupe takes
 * the first list entry as target, so ordering the representative first keeps a
 * stable target across passes (convergence) without a per-group flag.
 */
#define GET_DUPLICATE_EXTENTS						\
"with grp(digest, len) as ( "						\
"	select extents.digest, len from extents "			\
"	join files on fileid = id "					\
"	where dedupe_seq <= ?2 and (extents.digest, len) in ( "		\
"		select extents.digest, len from extents "		\
"		join files on fileid = id "				\
"		where dedupe_seq > ?1 and dedupe_seq <= ?2) "		\
"	group by extents.digest, len having count(*) > 1) "		\
"select extents.digest, fileid, loff, len, poff from extents "		\
"join files on fileid = id "						\
"where (extents.digest, len) in (select digest, len from grp) and ( "	\
"	(dedupe_seq > ?1 and dedupe_seq <= ?2) "			\
"	or extents.rowid in ( "						\
"		select min(e.rowid) from extents e "			\
"		join files f on e.fileid = f.id "			\
"		where f.dedupe_seq <= ?1 "				\
"		and (e.digest, e.len) in (select digest, len from grp) "\
"		group by e.digest, e.len)) "				\
"order by (dedupe_seq > ?1), fileid;"
	dbfile_prepare_stmt(get_duplicate_extents, GET_DUPLICATE_EXTENTS);

/*
 * Select duplicates, excluding future files.
 * Then, only keep duplicates if at least one entry is related to the
 * current pass: the (?1, ?2] range of dedupe generations. Loading many
 * generations per pass keeps the dedupe thread pool full instead of
 * draining it at every 1024-file scan batch.
 */
/*
 * A group whose copies span many scan generations was reprocessed once per
 * pass, dragging every already-deduped copy along each time (loaded and
 * re-fiemap-checked, only to be skipped). A pass only needs the copies new in
 * its generation range (?1, ?2] plus ONE already-deduped copy to act as the
 * dedupe target; the new copies converge on it, and the old copies - already
 * mutually shared from their own pass - never need reloading. `grp` is the set
 * of qualifying groups (a member in range, and >1 member overall).
 */
#define GET_DUPLICATE_FILES							\
"with grp(digest, size) as ( "							\
"	select digest, size from files "				\
"	where dedupe_seq <= ?2 and not (flags & 1) and (digest, size) in ( "	\
"		select digest, size from files "				\
"		where dedupe_seq > ?1 and dedupe_seq <= ?2 "			\
"		and not (flags & 1)) "						\
"	group by digest, size having count(*) > 1) "			\
"select id, size, digest, filename, dedupe_seq from files "			\
"where not (flags & 1) "						\
"and (digest, size) in (select digest, size from grp) and ( "		\
"	(dedupe_seq > ?1 and dedupe_seq <= ?2) "			\
"	or id in ( "							\
"		select min(id) from files "				\
"		where dedupe_seq <= ?1 and not (flags & 1) "		\
"		and (digest, size) in (select digest, size from grp) "	\
"		group by digest, size)) "				\
"order by (dedupe_seq > ?1), id;"
	dbfile_prepare_stmt(get_duplicate_files, GET_DUPLICATE_FILES);

#define GET_NONDUPE_EXTENTS						\
"select extents.loff, len, poff "					\
"FROM extents join files on files.id = extents.fileid "			\
"where files.id = ?1 and not exists "					\
"(SELECT 1 FROM extents as e where e.digest = extents.digest "		\
"and e.rowid <> extents.rowid);"
	dbfile_prepare_stmt(get_nondupe_extents, GET_NONDUPE_EXTENTS);

#define DELETE_FILE \
"delete from files where path_hash = ?1 and filename = ?2;"
	dbfile_prepare_stmt(delete_file, DELETE_FILE);

#define DELETE_FILE_BY_ID \
"delete from files where id = ?1;"
	dbfile_prepare_stmt(delete_file_by_id, DELETE_FILE_BY_ID);

#define SELECT_FILE_CHANGES						\
"select mtime, size, filename, id from files where ino = ?1 and subvol = ?2;"
	dbfile_prepare_stmt(select_file_changes, SELECT_FILE_CHANGES);

#define COUNT_B_HASHES "select COUNT(*) from blocks;"
	dbfile_prepare_stmt(count_b_hashes, COUNT_B_HASHES);

#define COUNT_E_HASHES "select COUNT(*) from extents;"
	dbfile_prepare_stmt(count_e_hashes, COUNT_E_HASHES);

#define COUNT_FILES "select COUNT(*) from files;"
	dbfile_prepare_stmt(count_files, COUNT_FILES);

#define GET_MAX_DEDUPE_SEQ "select max(dedupe_seq) from files;"
	dbfile_prepare_stmt(get_max_dedupe_seq, GET_MAX_DEDUPE_SEQ);

#define DELETE_UNSCANNED_FILES "delete from files where digest is NULL;"
	dbfile_prepare_stmt(delete_unscanned_files, DELETE_UNSCANNED_FILES);

#define RENAME_FILE							\
"update or replace files set filename = ?1, path_hash = ?2 where id = ?3;"
	dbfile_prepare_stmt(rename_file, RENAME_FILE);
	return result;

err:
	dbfile_close_handle(result);
	return NULL;
}

struct dbhandle *dbfile_open_handle(char *filename)
{
	return open_handle(filename, false);
}

/*
 * Open a hashfile for a read-only report. Issues no writes, so it is safe to
 * run concurrently with a deduping oans (no WAL write-lock contention) and
 * never modifies or recreates the file. Returns NULL if the file is missing or
 * is not an oans hashfile.
 */
struct dbhandle *dbfile_open_handle_ro(char *filename)
{
	return open_handle(filename, true);
}

void dbfile_close_handle(struct dbhandle *db)
{
	if(db) {
		/* struct stmts is a named array of sqlite3_stmt*
		 * let's iterate over all unnamed elements and
		 * finalize each of them
		 */
		sqlite3_stmt **stmts = (sqlite3_stmt**)&(db->stmts);

		int len = sizeof(struct stmts) / sizeof(sqlite3_stmt*);
		for (int i = 0; i < len; i++) {
			sqlite3_finalize(stmts[i]);
		}

		sqlite3_close(db->db);
		free(db);
	}
}

/*
 * dbfile_close_handle takes struct dbhandle*.
 * we need a function that takes void* so we
 * can pass it to register_cleanup without
 * causing UB.
 */
static void cleanup_dbhandle(void *db)
{
	dbfile_close_handle(db);
}

struct dbhandle *dbfile_open_handle_thread(struct threads_pool *pool)
{
	struct dbhandle *db;
	dbfile_lock();
	db = dbfile_open_handle(options.hashfile);
	dbfile_unlock();

	if (db)
		register_cleanup(pool, (void*)&cleanup_dbhandle, db);
	return db;
}

int dbfile_begin_trans(sqlite3 *db)
{
	int ret;

	ret = sqlite3_exec(db, "begin transaction", NULL, NULL, NULL);
	if (ret)
		perror_sqlite(ret, "starting transaction");
	return ret;
}

int dbfile_update_extent_poff(struct dbhandle *db, int64_t fileid,
				uint64_t loff, uint64_t poff)
{
	int ret;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.update_extent_poff;

	ret = sqlite3_bind_int64(stmt, 1, poff);
	if (ret)
		goto bind_error;

	ret = sqlite3_bind_int64(stmt, 2, fileid);
	if (ret)
		goto bind_error;

	ret = sqlite3_bind_int64(stmt, 3, loff);
	if (ret)
		goto bind_error;

	ret = sqlite3_step(stmt);
	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "executing statement");
		return ret;
	}

	ret = 0;
bind_error:
	if (ret)
		perror_sqlite(ret, "binding values");

	return ret;
}

int dbfile_commit_trans(sqlite3 *db)
{
	int ret;

	ret = sqlite3_exec(db, "commit transaction", NULL, NULL, NULL);
	if (ret)
		perror_sqlite(ret, "committing transaction");
	return ret;
}

int dbfile_abort_trans(sqlite3 *db)
{
	int ret;

	ret = sqlite3_exec(db, "rollback transaction", NULL, NULL, NULL);
	if (ret)
		perror_sqlite(ret, "aborting transaction");
	return ret;
}

static int sync_config_text(sqlite3_stmt *stmt, const char *key, char *val,
			    int len)
{
	int ret;

	ret = sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
	if (ret)
		goto out;
	ret = sqlite3_bind_text(stmt, 2, val, len, SQLITE_TRANSIENT);
	if (ret)
		goto out;
	ret = sqlite3_step(stmt);
	if (ret != SQLITE_DONE)
		goto out;
	sqlite3_reset(stmt);

	ret = 0;
out:
	return ret;
}

static int sync_config_int(sqlite3_stmt *stmt, const char *key, int64_t val)
{
	int ret;

	ret = sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
	if (ret)
		goto out;
	ret = sqlite3_bind_int64(stmt, 2, val);
	if (ret)
		goto out;
	ret = sqlite3_step(stmt);
	if (ret != SQLITE_DONE)
		goto out;
	sqlite3_reset(stmt);

	ret = 0;
out:
	return ret;
}

int __dbfile_sync_config(sqlite3 *db, struct dbfile_config *cfg)
{
	int ret = 0;
	char uuid[37]; /* 36-bytes uuid + \0 */
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;

	ret = sqlite3_prepare_v2(db,
				 "insert or replace into config VALUES (?1, ?2)", -1,
				 &stmt, NULL);
	if (ret) {
		perror_sqlite(ret, "preparing statement");
		return ret;
	}

	ret = sync_config_text(stmt, "hash_type", cfg->hash_type, 8);
	if (ret)
		goto out;

	ret = sync_config_int(stmt, "block_size", cfg->blocksize);
	if (ret)
		goto out;

	ret = sync_config_int(stmt, "dedupe_sequence", cfg->dedupe_seq);
	if (ret)
		goto out;

	ret = sync_config_int(stmt, "version_minor", cfg->minor);
	if (ret)
		goto out;

	uuid_unparse(cfg->fs_uuid, uuid);
	ret = sync_config_text(stmt, "fs_uuid", uuid, 36);
	if (ret)
		goto out;

	/*
	 * Always write version_major last so we have an easy check
	 * whether the config table was fully written.
	 */
	ret = sync_config_int(stmt, "version_major", cfg->major);
	if (ret)
		goto out;

out:
	if (ret) {
		perror_sqlite(ret, "binding");
	}

	return ret;
}

int dbfile_sync_config(struct dbhandle *db, struct dbfile_config *cfg)
{
	return __dbfile_sync_config(db->db, cfg);
}

static int get_config_int(sqlite3_stmt *stmt, const char *name, int *val);
static int get_config_int64(sqlite3_stmt *stmt, const char *name, int64_t *val);
static int sync_config_int(sqlite3_stmt *stmt, const char *key, int64_t val);

/* Delete every row of `table`, then insert each of `n` strings in order. */
static int replace_string_rows(sqlite3 *db, const char *insert_sql,
			       const char *delete_sql, char **vals, int n)
{
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;
	int ret, i;

	ret = sqlite3_exec(db, delete_sql, NULL, NULL, NULL);
	if (ret)
		return ret;

	ret = sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL);
	if (ret)
		return ret;

	for (i = 0; i < n; i++) {
		sqlite3_reset(stmt);
		ret = sqlite3_bind_text(stmt, 1, vals[i], -1, SQLITE_STATIC);
		if (!ret && sqlite3_step(stmt) != SQLITE_DONE)
			ret = -1;
		if (ret)
			return ret;
	}
	return 0;
}

int dbfile_store_scan_config(struct dbhandle *dbh, const struct scan_config *sc)
{
	sqlite3 *db = dbh->db;
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;
	int ret;

	ret = sqlite3_exec(db, "begin transaction", NULL, NULL, NULL);
	if (ret)
		return ret;

	ret = sqlite3_prepare_v2(db,
		"insert or replace into config values (?1, ?2)", -1, &stmt, NULL);
	if (ret)
		goto err;

	/* A marker row lets load distinguish "stored" from "never stored". */
	if ((ret = sync_config_int(stmt, "scan_config", 1)) ||
	    (ret = sync_config_int(stmt, "opt_run_dedupe", sc->run_dedupe)) ||
	    (ret = sync_config_int(stmt, "opt_recurse", sc->recurse)) ||
	    (ret = sync_config_int(stmt, "opt_skip_zeroes", sc->skip_zeroes)) ||
	    (ret = sync_config_int(stmt, "opt_only_whole_files", sc->only_whole_files)) ||
	    (ret = sync_config_int(stmt, "opt_do_block_hash", sc->do_block_hash)) ||
	    (ret = sync_config_int(stmt, "opt_dedupe_same_file", sc->dedupe_same_file)) ||
	    (ret = sync_config_int(stmt, "opt_min_filesize", (int64_t)sc->min_filesize)))
		goto err;

	ret = replace_string_rows(db,
		"insert into scan_roots(path) values (?1)",
		"delete from scan_roots", sc->roots, sc->nroots);
	if (ret)
		goto err;

	ret = replace_string_rows(db,
		"insert into scan_excludes(pattern) values (?1)",
		"delete from scan_excludes", sc->excludes, sc->nexcludes);
	if (ret)
		goto err;

	ret = sqlite3_exec(db, "commit transaction", NULL, NULL, NULL);
	if (ret)
		goto err;
	return 0;

err:
	perror_sqlite(ret, "storing scan config");
	sqlite3_exec(db, "rollback transaction", NULL, NULL, NULL);
	return ret ? ret : -1;
}

/* Read all rows of a single-TEXT-column table into a malloc'd char* array. */
static int load_string_rows(sqlite3 *db, const char *sql,
			    char ***out, int *nout)
{
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;
	char **arr = NULL;
	int n = 0, cap = 0, ret;

	*out = NULL;
	*nout = 0;

	ret = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
	if (ret)
		return ret;

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		const unsigned char *txt = sqlite3_column_text(stmt, 0);

		if (n == cap) {
			cap = cap ? cap * 2 : 8;
			arr = realloc(arr, cap * sizeof(*arr));
			abort_on(!arr);
		}
		arr[n++] = strdup(txt ? (const char *)txt : "");
	}
	if (ret != SQLITE_DONE) {
		for (int i = 0; i < n; i++)
			free(arr[i]);
		free(arr);
		return ret;
	}

	*out = arr;
	*nout = n;
	return 0;
}

int dbfile_load_scan_config(struct dbhandle *dbh, struct scan_config *sc)
{
	sqlite3 *db = dbh->db;
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;
	int present = 0, ret;

	memset(sc, 0, sizeof(*sc));

#define SELECT_CONFIG "select keyval from config where keyname=?1;"
	ret = sqlite3_prepare_v2(db, SELECT_CONFIG, -1, &stmt, NULL);
	if (ret)
		return ret;

	ret = get_config_int(stmt, "scan_config", &present);
	if (ret)
		return ret;
	if (!present)
		return 0;	/* no stored configuration */

	get_config_int(stmt, "opt_run_dedupe", &sc->run_dedupe);
	get_config_int(stmt, "opt_recurse", &sc->recurse);
	get_config_int(stmt, "opt_skip_zeroes", &sc->skip_zeroes);
	get_config_int(stmt, "opt_only_whole_files", &sc->only_whole_files);
	get_config_int(stmt, "opt_do_block_hash", &sc->do_block_hash);
	get_config_int(stmt, "opt_dedupe_same_file", &sc->dedupe_same_file);
	{
		int64_t mfs = 0;

		ret = get_config_int64(stmt, "opt_min_filesize", &mfs);
		if (ret)
			return ret;
		sc->min_filesize = (uint64_t)mfs;
	}

	ret = load_string_rows(db, "select path from scan_roots order by rowid;",
			       &sc->roots, &sc->nroots);
	if (ret)
		return ret;
	ret = load_string_rows(db,
			       "select pattern from scan_excludes order by rowid;",
			       &sc->excludes, &sc->nexcludes);
	if (ret)
		return ret;

	return 1;
}

void scan_config_free(struct scan_config *sc)
{
	int i;

	for (i = 0; i < sc->nroots; i++)
		free(sc->roots[i]);
	free(sc->roots);
	for (i = 0; i < sc->nexcludes; i++)
		free(sc->excludes[i]);
	free(sc->excludes);
	memset(sc, 0, sizeof(*sc));
}

int dbfile_record_run(struct dbhandle *dbh, const struct run_record *r)
{
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;
	int ret;

	ret = sqlite3_prepare_v2(dbh->db,
		"insert into run_history(ts, duration_ms, files_scanned, "
		"reclaimed, groups, kernel_bytes, deduped) "
		"values (?1, ?2, ?3, ?4, ?5, ?6, ?7)", -1, &stmt, NULL);
	if (ret)
		goto out;

	sqlite3_bind_int64(stmt, 1, r->ts);
	sqlite3_bind_int64(stmt, 2, r->duration_ms);
	sqlite3_bind_int64(stmt, 3, r->files_scanned);
	sqlite3_bind_int64(stmt, 4, r->reclaimed);
	sqlite3_bind_int64(stmt, 5, r->groups);
	sqlite3_bind_int64(stmt, 6, r->kernel_bytes);
	sqlite3_bind_int64(stmt, 7, r->deduped);

	if (sqlite3_step(stmt) != SQLITE_DONE)
		ret = -1;
out:
	if (ret)
		perror_sqlite(ret, "recording run history");
	return ret;
}

int dbfile_get_run_summary(struct dbhandle *dbh, struct run_summary *s)
{
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;
	int ret;

	memset(s, 0, sizeof(*s));
	ret = sqlite3_prepare_v2(dbh->db,
		"select count(*), ifnull(sum(reclaimed),0), "
		"ifnull(sum(files_scanned),0), ifnull(min(ts),0), "
		"ifnull(max(ts),0) from run_history", -1, &stmt, NULL);
	if (ret) {
		perror_sqlite(ret, "reading run summary");
		return ret;
	}
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		s->runs = sqlite3_column_int64(stmt, 0);
		s->total_reclaimed = sqlite3_column_int64(stmt, 1);
		s->total_files = sqlite3_column_int64(stmt, 2);
		s->first_ts = sqlite3_column_int64(stmt, 3);
		s->last_ts = sqlite3_column_int64(stmt, 4);
	}
	return 0;
}

static int __dbfile_count_rows(sqlite3_stmt *s, uint64_t *num)
{
	int ret;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = s;

	ret = sqlite3_step(stmt);
	if (ret != SQLITE_ROW) {
		perror_sqlite(ret, "retrieving count from table (step)");
		return ret;
	}

	*num = sqlite3_column_int64(stmt, 0);
	return 0;
}

int dbfile_get_stats(struct dbhandle *db, struct dbfile_stats *stats)
{
	int ret = 0;
	ret = __dbfile_count_rows(db->stmts.count_b_hashes, &(stats->num_b_hashes));
	if (ret)
		return ret;

	ret = __dbfile_count_rows(db->stmts.count_e_hashes, &(stats->num_e_hashes));
	if (ret)
		return ret;

	ret = __dbfile_count_rows(db->stmts.count_files, &(stats->num_files));
	if (ret)
		return ret;

	return ret;
}

/*
 * VACUUM reclaims free pages, but SQLite reuses them - so under normal scanning
 * the freelist stays near empty and a full-database rewrite would reclaim
 * nothing. It only fills up after a large prune (e.g. many deleted files
 * removed from the hashfile), which is exactly when the rewrite pays off. So
 * VACUUM only when a meaningful fraction of the file is actually free.
 *
 * The exception is a from-scratch build (hashfile_rebuilt): it has no freelist,
 * but its pages sit at insert density - the random-key path_hash/digest indexes
 * fill to only ~2/3 - so a one-off VACUUM meaningfully compacts it.
 */
#define VACUUM_FREE_PCT		25

void dbfile_maybe_vacuum(struct dbhandle *db)
{
	uint64_t freelist, total;
	int ret;

	freelist = dbfile_query_u64(db->db, "PRAGMA freelist_count");
	total = dbfile_query_u64(db->db, "PRAGMA page_count");

	if (!hashfile_rebuilt &&
	    (total == 0 || freelist * 100 < total * VACUUM_FREE_PCT))
		return;

	if (hashfile_rebuilt) {
		qprintf("Compacting the rebuilt hashfile ...\n");
	} else {
		vprintf("Vacuuming hashfile: %"PRIu64" of %"PRIu64" pages free\n",
			freelist, total);
	}

	/* Maintenance only: a failure here must not fail the run. */
	ret = sqlite3_exec(db->db, "VACUUM", NULL, NULL, NULL);
	if (ret)
		perror_sqlite(ret, "vacuuming hashfile");
}

static int get_config_int(sqlite3_stmt *stmt, const char *name, int *val)
{
	int64_t v = val ? *val : 0;	/* preserve caller's default if no row */
	int ret = get_config_int64(stmt, name, val ? &v : NULL);

	if (val)
		*val = (int)v;
	return ret;
}

static int get_config_int64(sqlite3_stmt *stmt, const char *name, int64_t *val)
{
	int ret;

	if (!val)
		return 0;

	ret = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	if (ret) {
		perror_sqlite(ret, "retrieving row from config table (bind)");
		return ret;
	}

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW)
		*val = sqlite3_column_int64(stmt, 0);

	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "retrieving row from config table (step)");
		return ret;
	}

	sqlite3_reset(stmt);
	return 0;
}

static int get_config_text(sqlite3_stmt *stmt, const char *name, char *val, int len)
{
	int ret;
	const unsigned char *local;

	if (!val)
		return 0;

	ret = sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
	if (ret) {
		perror_sqlite(ret, "retrieving row from config table (bind)");
		return ret;
	}

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		local = sqlite3_column_text(stmt, 0);
		memcpy(val, local, len);
	}

	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "retrieving row from config table (step)");
		return ret;
	}

	sqlite3_reset(stmt);

	return 0;
}

static int __dbfile_get_config(sqlite3 *db, struct dbfile_config *cfg)
{
	int ret;
	/*
	 * Zero-initialised so the buffer is always NUL-terminated: get_config_text
	 * memcpy()s exactly `len` (36) bytes without terminating, and if the
	 * config row is absent it writes nothing at all - either way uuid_parse()
	 * below would otherwise strlen() past uninitialised bytes.
	 */
	char uuid[37] = "";	/* 36-byte uuid + NUL */
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;

#define SELECT_CONFIG "select keyval from config where keyname=?1;"
	ret = sqlite3_prepare_v2(db, SELECT_CONFIG, -1, &stmt, NULL);
	if (ret) {
		perror_sqlite(ret, "preparing statement");
		goto out;
	}

	ret = get_config_int(stmt, "block_size", (int *)&cfg->blocksize);
	if (ret)
		goto out;

	ret = get_config_text(stmt, "hash_type", cfg->hash_type, 8);
	if (ret)
		goto out;

	ret = get_config_int(stmt, "version_major", &cfg->major);
	if (ret)
		goto out;

	ret = get_config_int(stmt, "version_minor", &cfg->minor);
	if (ret)
		goto out;

	ret = get_config_int(stmt, "dedupe_sequence", (int *)&cfg->dedupe_seq);
	if (ret)
		goto out;

	/* Optional; stays 0 (dbfile_config_defaults) when --autotune never ran. */
	ret = get_config_int(stmt, AUTOTUNE_CONFIG_KEY,
			     (int *)&cfg->autotune_io_threads);
	if (ret)
		goto out;

	ret = get_config_text(stmt, "fs_uuid", uuid, 36);
	if (ret)
		goto out;

	uuid_parse(uuid, cfg->fs_uuid);

out:
	if (ret != 0)
		perror_sqlite(ret, "__dbfile_get_config");
	return ret;
}

int dbfile_get_config(sqlite3 *db, struct dbfile_config *cfg)
{
	dbfile_config_defaults(cfg);
	return __dbfile_get_config(db, cfg);
}

int dbfile_set_config_int(struct dbhandle *db, const char *key, int64_t val)
{
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;
	int ret;

	ret = sqlite3_prepare_v2(db->db,
				 "insert or replace into config values (?1, ?2)",
				 -1, &stmt, NULL);
	if (ret) {
		perror_sqlite(ret, "preparing statement");
		return ret;
	}

	ret = sync_config_int(stmt, key, val);
	if (ret)
		perror_sqlite(ret, "dbfile_set_config_int");
	return ret;
}

/* Returns 0 on error, and the inserted rowid on success */
int64_t dbfile_store_file_info(struct dbhandle *db, struct file *dbfile)
{
	int ret;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.write_file;

	ret = sqlite3_bind_int64(stmt, 1, dbfile->ino);
	if (ret)
		goto bind_error;

	ret = sqlite3_bind_int64(stmt, 2, dbfile->subvol);
	if (ret)
		goto bind_error;

	ret = sqlite3_bind_text(stmt, 3, dbfile->filename, -1, SQLITE_STATIC);
	if (ret)
		goto bind_error;

	ret = sqlite3_bind_int64(stmt, 4, csum_path(dbfile->filename));
	if (ret)
		goto bind_error;

	ret = sqlite3_bind_int64(stmt, 5, dbfile->size);
	if (ret)
		goto bind_error;

	ret = sqlite3_bind_int64(stmt, 6, dbfile->mtime);
	if (ret)
		goto bind_error;

	ret = sqlite3_bind_int(stmt, 7, dbfile->dedupe_seq);
	if (ret)
		goto bind_error;

	ret = sqlite3_step(stmt);
	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "executing sql");
		goto out_error;
	}

	return sqlite3_last_insert_rowid(db->db);

bind_error:
	if (ret)
		perror_sqlite(ret, "binding values");
out_error:
	return 0;
}

static int __dbfile_remove_file_hashes(sqlite3_stmt *stmt, int64_t fileid)
{
	int ret;

	ret = sqlite3_bind_int64(stmt, 1, fileid);
	if (ret) {
		perror_sqlite(ret, "binding fileid");
		goto out;
	}

	ret = sqlite3_step(stmt);
	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "removing hashes statement");
		goto out;
	}

	ret = 0;
out:
	return ret;
}

int dbfile_remove_extent_hashes(struct dbhandle *db, int64_t fileid)
{
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.remove_extent_hashes;
	return __dbfile_remove_file_hashes(stmt, fileid);
}

int dbfile_remove_hashes(struct dbhandle *db, int64_t fileid)
{
	int ret = 0;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *b_stmt = db->stmts.remove_block_hashes;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *e_stmt = db->stmts.remove_extent_hashes;

	ret += __dbfile_remove_file_hashes(b_stmt, fileid);
	ret += __dbfile_remove_file_hashes(e_stmt, fileid);

	return ret;
}

int dbfile_store_block_hashes(struct dbhandle *db, int64_t fileid,
				uint64_t nb_hash, struct block_csum *hashes)
{
	int ret;
	uint64_t i;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.insert_block;

	for (i = 0; i < nb_hash; i++) {
		ret = sqlite3_bind_int64(stmt, 1, fileid);
		if (ret)
			goto bind_error;

		ret = sqlite3_bind_int64(stmt, 2, hashes[i].loff);
		if (ret)
			goto bind_error;

		ret = sqlite3_bind_blob(stmt, 3, hashes[i].digest, DIGEST_LEN,
					SQLITE_STATIC);
		if (ret)
			goto bind_error;

		ret = sqlite3_step(stmt);
		if (ret != SQLITE_DONE) {
			perror_sqlite(ret, "executing statement");
			goto out_error;
		}

		sqlite3_reset(stmt);
	}

	ret = 0;
bind_error:
	if (ret)
		perror_sqlite(ret, "binding values");
out_error:

	return ret;
}

int dbfile_update_scanned_file(struct dbhandle *db, int64_t fileid,
				unsigned char *digest, unsigned int flags)
{
	int ret;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.update_scanned_file;

	ret = sqlite3_bind_blob(stmt, 1, digest, DIGEST_LEN,
				SQLITE_STATIC);
	if (ret)
		goto bind_error;

	ret = sqlite3_bind_int64(stmt, 2, flags);
	if (ret)
		goto bind_error;

	ret = sqlite3_bind_int64(stmt, 3, fileid);
	if (ret)
		goto bind_error;

	ret = sqlite3_step(stmt);
	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "executing statement");
		goto out_error;
	}

	ret = 0;
bind_error:
	if (ret)
		perror_sqlite(ret, "binding values");
out_error:
	return ret;
}

int dbfile_store_extent_hashes(struct dbhandle *db, int64_t fileid,
				uint64_t nb_hash, struct extent_csum *hashes)
{
	int ret;
	uint64_t i;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.insert_extent;

	for (i = 0; i < nb_hash; i++) {
		/*
		 * If len == 0, then this extent was never scanned and
		 * must be skipped.
		 */
		if (hashes[i].len == 0)
			continue;

		ret = sqlite3_bind_int64(stmt, 1, fileid);
		if (ret)
			goto bind_error;

		ret = sqlite3_bind_int64(stmt, 2, hashes[i].loff);
		if (ret)
			goto bind_error;

		ret = sqlite3_bind_int64(stmt, 3, hashes[i].poff);
		if (ret)
			goto bind_error;

		ret = sqlite3_bind_int64(stmt, 4, hashes[i].len);
		if (ret)
			goto bind_error;

		ret = sqlite3_bind_blob(stmt, 5, hashes[i].digest, DIGEST_LEN,
					SQLITE_STATIC);
		if (ret)
			goto bind_error;

		ret = sqlite3_step(stmt);
		if (ret != SQLITE_DONE) {
			perror_sqlite(ret, "executing statement");
			goto out_error;
		}

		sqlite3_reset(stmt);
	}

	ret = 0;
bind_error:
	if (ret)
		perror_sqlite(ret, "binding values");
out_error:

	return ret;
}

int dbfile_load_one_filerec(struct dbhandle *db, int64_t fileid,
				   struct filerec **file)
{
	int ret;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.load_filerec;
	const unsigned char *filename;
	uint64_t size;

	*file = NULL;

	ret = sqlite3_bind_int64(stmt, 1, fileid);
	if (ret) {
		perror_sqlite(ret, "binding fileid");
		return ret;
	}

	ret = sqlite3_step(stmt);
	if (ret == SQLITE_DONE) {
		dprintf("dbfile_load_one_filerec: no file found in hashdb: fileid = %lu\n", fileid);
		return 0;
	}

	if (ret != SQLITE_ROW) {
		perror_sqlite(ret, "executing statement");
		return ret;
	}

	filename = sqlite3_column_text(stmt, 0);
	size = sqlite3_column_int64(stmt, 1);

	*file = filerec_new((const char *)filename, fileid, size);
	if (!*file)
		return ENOMEM;

	return 0;
}

/* Return the cached filerec for fileid, loading it from the db if needed. */
static int find_or_load_filerec(struct dbhandle *db, int64_t fileid,
				struct filerec **file)
{
	int ret;

	*file = filerec_find(fileid);
	if (*file)
		return 0;

	ret = dbfile_load_one_filerec(db, fileid, file);
	if (ret)
		eprintf("Error loading filerec (%"PRIu64") from db\n", fileid);
	return ret;
}

int dbfile_load_block_hashes(struct dbhandle *db, struct hash_tree *hash_tree,
			     unsigned int seq_lo, unsigned int seq_hi)
{
	int ret;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.get_duplicate_blocks;
	uint64_t loff;
	int64_t fileid;
	unsigned char *digest;
	struct filerec *file;

	ret = sqlite3_bind_int64(stmt, 1, seq_lo);
	if (!ret)
		ret = sqlite3_bind_int64(stmt, 2, seq_hi);
	if (ret) {
		perror_sqlite(ret, "binding value");
		return ret;
	}

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		digest = (unsigned char *)sqlite3_column_blob(stmt, 0);
		fileid = sqlite3_column_int64(stmt, 1);
		loff = sqlite3_column_int64(stmt, 2);

		ret = find_or_load_filerec(db, fileid, &file);
		if (ret)
			return ret;

		ret = insert_hashed_block(hash_tree, digest, file, loff);
		if (ret)
			return ENOMEM;
	}
	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "looking up hash");
		return ret;
	}

	sort_file_hash_heads(hash_tree);

	return 0;
}

int dbfile_load_extent_hashes(struct dbhandle *db, struct results_tree *res,
			      unsigned int seq_lo, unsigned int seq_hi)
{
	int ret;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.get_duplicate_extents;
	uint64_t loff, poff, len;
	int64_t fileid;
	unsigned char *digest;
	struct filerec *file;

	ret = sqlite3_bind_int64(stmt, 1, seq_lo);
	if (!ret)
		ret = sqlite3_bind_int64(stmt, 2, seq_hi);
	if (ret) {
		perror_sqlite(ret, "binding value");
		return ret;
	}

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		digest = (unsigned char *)sqlite3_column_blob(stmt, 0);
		fileid = sqlite3_column_int64(stmt, 1);
		loff = sqlite3_column_int64(stmt, 2);
		len = sqlite3_column_int64(stmt, 3);
		poff = sqlite3_column_int64(stmt, 4);

		ret = find_or_load_filerec(db, fileid, &file);
		if (ret)
			return ret;

		ret = insert_one_result(res, digest, file, loff, len, poff,
					false);
		if (ret)
			return ENOMEM;
	}
	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "looking up hash");
		return ret;
	}

	return 0;
}

int dbfile_load_nondupe_file_extents(struct dbhandle *db, struct filerec *file,
				     struct file_extent **ret_extents,
				     unsigned int *num_extents)
{
	int ret;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.get_nondupe_extents;
	uint64_t count = 0, capacity = 0;
	struct file_extent *extents = NULL;

	ret = sqlite3_bind_int64(stmt, 1, file->fileid);
	if (ret) {
		perror_sqlite(ret, "binding values");
		goto out;
	}

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		if (count == capacity) {
			struct file_extent *tmp;

			/* Grow geometrically to avoid O(n^2) copying. */
			capacity = capacity ? capacity * 2 : 16;
			tmp = realloc(extents, capacity * sizeof(struct file_extent));
			if (!tmp) {
				ret = ENOMEM;
				goto out;
			}
			extents = tmp;
		}

		extents[count].loff = sqlite3_column_int64(stmt, 0);
		extents[count].len = sqlite3_column_int64(stmt, 1);
		extents[count].poff = sqlite3_column_int64(stmt, 2);

		count++;
	}

	*num_extents = count;
	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "stepping nondupe extents statement");
		goto out;
	}
	ret = 0;
out:
	*ret_extents = extents;
	if (ret && extents)
		free(extents);
	return ret;
}

static int iter_cb(void *priv, int argc, char **argv,
		char **column [[maybe_unused]])
{
	iter_files_func func = priv;

	abort_on(argc != 3);
	func(argv[0], argv[1], argv[2]);
	return 0;
}

int dbfile_iter_files(struct dbhandle *db, iter_files_func func)
{
	int ret;

#define	LIST_FILES	"select filename, ino, subvol from files;"
	ret = sqlite3_exec(db->db, LIST_FILES, iter_cb, func, NULL);
	if (ret) {
		perror_sqlite(ret, "Running sql to list files.");
		return ret;
	}

	return 0;
}

int dbfile_remove_file(struct dbhandle *db, const char *filename)
{
	int ret;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.delete_file;

	dprintf("Remove file \"%s\" from the db\n", filename);

	ret = sqlite3_bind_int64(stmt, 1, csum_path(filename));
	if (ret) {
		perror_sqlite(ret, "binding path_hash for sql");
		return ret;
	}

	ret = sqlite3_bind_text(stmt, 2, filename, -1, SQLITE_STATIC);
	if (ret) {
		perror_sqlite(ret, "binding filename for sql");
		return ret;
	}

	ret = sqlite3_step(stmt);
	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "executing sql");
		return ret;
	}

	return 0;
}

/* Check if the data in the hashfile is in synced with the disk.
 * Returns false only if they match.
 * Returns true if not, or if there is not data found, or on error.
 */
int dbfile_describe_file(struct dbhandle *db, uint64_t ino, uint64_t subvol,
				struct file *dbfile)
{
	int ret;
	char *buf;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.select_file_changes;

	/* in-memory databases has no wal support,
	 * so we must do the lock by ourselves
	 */
	if (!options.hashfile)
		dbfile_lock();

	ret = sqlite3_bind_int64(stmt, 1, ino);
	if (ret) {
		perror_sqlite(ret, "binding values");
		goto out;
	}

	ret = sqlite3_bind_int64(stmt, 2, subvol);
	if (ret) {
		perror_sqlite(ret, "binding values");
		goto out;
	}

	ret = sqlite3_step(stmt);
	if (ret == SQLITE_DONE) {
		ret = 0;
		goto out;
	}

	if (ret != SQLITE_ROW) {
		perror_sqlite(ret, "fetching a file");
		goto out;
	}

	dbfile->mtime = sqlite3_column_int64(stmt, 0);
	dbfile->size = sqlite3_column_int64(stmt, 1);

	buf = (char *)sqlite3_column_text(stmt, 2);
	strncpy(dbfile->filename, buf, PATH_MAX);

	dbfile->id = sqlite3_column_int64(stmt, 3);

	ret = 0;

out:
	if (!options.hashfile)
		dbfile_unlock();
	return ret;
}

int dbfile_load_same_files(struct dbhandle *db, struct results_tree *res,
			   unsigned int seq_lo, unsigned int seq_hi)
{
	int ret;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.get_duplicate_files;
	uint64_t size;
	int64_t fileid;
	unsigned char *digest;
	struct filerec *file;
	const unsigned char *filename;

	ret = sqlite3_bind_int64(stmt, 1, seq_lo);
	if (!ret)
		ret = sqlite3_bind_int64(stmt, 2, seq_hi);
	if (ret) {
		perror_sqlite(ret, "binding value");
		return ret;
	}

	while ((ret = sqlite3_step(stmt)) == SQLITE_ROW) {
		unsigned int seq;
		bool is_anchor;

		fileid = sqlite3_column_int64(stmt, 0);
		size = sqlite3_column_int64(stmt, 1);
		digest = (unsigned char *)sqlite3_column_blob(stmt, 2);
		filename = sqlite3_column_text(stmt, 3);
		seq = sqlite3_column_int64(stmt, 4);
		/* A member from an earlier pass (seq <= seq_lo) is the anchor:
		 * the group must keep it as target so copies keep converging. */
		is_anchor = seq <= seq_lo;

		file = filerec_find(fileid);
		if (!file) {
			file = filerec_new((const char *)filename, fileid, size);
			if (!file)
				return ENOMEM;
		}

		ret = insert_one_result(res, digest, file, 0, size, 0, is_anchor);
		if (ret)
			return ENOMEM;
	}
	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "looking up hash");
		return ret;
	}

	return 0;
}

int dbfile_rename_file(struct dbhandle *db, int64_t fileid, char *path)
{
	int ret;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.rename_file;

	ret = sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
	if (ret) {
		perror_sqlite(ret, "binding values");
		return ret;
	}

	ret = sqlite3_bind_int64(stmt, 2, csum_path(path));
	if (ret) {
		perror_sqlite(ret, "binding values");
		return ret;
	}

	ret = sqlite3_bind_int64(stmt, 3, fileid);
	if (ret) {
		perror_sqlite(ret, "binding values");
		return ret;
	}

	ret = sqlite3_step(stmt);
	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "renaming a file");
		return ret;
	}

	return 0;
}

void dbfile_set_gdb(struct dbhandle *db)
{
	gdb = db;
}

unsigned int get_max_dedupe_seq(struct dbhandle *db)
{
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.get_max_dedupe_seq;

	int ret = sqlite3_step(stmt);
	if (ret != SQLITE_ROW) {
		eprintf("error %d, get max dedupe seq: %s\n",
			ret, sqlite3_errstr(ret));
		return 0;
	}

	return sqlite3_column_int64(stmt, 0);
}

uint64_t dbfile_count_dupe_groups(struct dbhandle *db, bool whole_file_only)
{
	uint64_t files, extents;

	files = dbfile_query_u64(db->db,
		"select count(*) from (select 1 from files "
		"where digest is not null and not (flags & 1) "
		"group by digest, size having count(*) > 1)");
	if (whole_file_only)
		return files;

	extents = dbfile_query_u64(db->db,
		"select count(*) from (select 1 from extents "
		"group by digest, len having count(*) > 1)");
	/*
	 * The whole-file and extent groups overlap heavily (a duplicate file is
	 * also a set of duplicate extents), so summing overshoots. The larger of
	 * the two is a closer, under-biased estimate for the bar; the caller
	 * clamps the total up if the running count exceeds it.
	 */
	return files > extents ? files : extents;
}

/*
 * Remove entries from the files table that were listed but never csummed, i.e.
 * whose digest is still NULL. This happens when a previous run was interrupted
 * (e.g. ctrl^C) after inserting a file record but before storing its hashes.
 *
 * Files deleted from disk are handled separately by
 * dbfile_prune_missing_files() (they keep a valid digest, so they are not
 * caught here).
 */
int dbfile_prune_unscanned_files(struct dbhandle *db)
{
	int ret;
	_cleanup_(sqlite3_reset_stmt) sqlite3_stmt *stmt = db->stmts.delete_unscanned_files;

	ret = sqlite3_step(stmt);
	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "executing sql");
		return ret;
	}

	return 0;
}

/*
 * Drop rows for files that no longer exist on disk (deleted since they were
 * scanned). Runs automatically after a scan. It is stat-based, not
 * "delete everything not walked this run": a row is removed only when its path
 * genuinely resolves to ENOENT, so scanning a subset of the tree (or sharing
 * one hashfile across several trees) never prunes files that still exist but
 * were simply out of scope this run. Extent/block hashes cascade away via the
 * ON DELETE CASCADE foreign key. Returns the number of files pruned, or -1 on
 * error.
 *
 * seen(id) is an optional "this row's file was confirmed on disk this run"
 * oracle (the scan's seen-set): rows it accepts are skipped without a stat(),
 * so the common nothing-deleted case does no stat()s at all. Pass NULL to
 * stat() every row.
 */
int64_t dbfile_prune_missing_files(struct dbhandle *db, bool (*seen)(int64_t))
{
	sqlite3_stmt *sel = NULL;
	sqlite3_stmt *del = db->stmts.delete_file_by_id;
	int64_t *gone = NULL;
	size_t n = 0, cap = 0;
	int64_t removed = -1;
	int ret;

	ret = sqlite3_prepare_v2(db->db, "select id, filename from files;",
				 -1, &sel, NULL);
	if (ret) {
		perror_sqlite(ret, "preparing prune-missing query");
		return -1;
	}

	/* Collect the ids first, then delete: don't mutate the table mid-scan. */
	while ((ret = sqlite3_step(sel)) == SQLITE_ROW) {
		int64_t id = sqlite3_column_int64(sel, 0);
		const char *fn;
		struct stat st;

		/* The walk already confirmed this file on disk - skip the stat. */
		if (seen && seen(id))
			continue;

		fn = (const char *)sqlite3_column_text(sel, 1);
		if (!fn || stat(fn, &st) == 0)
			continue;
		/* Only ENOENT/ENOTDIR mean "gone"; keep rows on EACCES, EIO, etc. */
		if (errno != ENOENT && errno != ENOTDIR)
			continue;

		if (n == cap) {
			size_t ncap = cap ? cap * 2 : 512;
			int64_t *tmp = realloc(gone, ncap * sizeof(*gone));
			if (!tmp) {
				eprintf("Out of memory pruning missing files.\n");
				goto out;
			}
			gone = tmp;
			cap = ncap;
		}
		gone[n++] = id;
	}
	if (ret != SQLITE_DONE) {
		perror_sqlite(ret, "scanning files to prune");
		goto out;
	}
	sqlite3_finalize(sel);
	sel = NULL;

	if (n == 0) {
		removed = 0;
		goto out;
	}

	if (dbfile_begin_trans(db->db))
		goto out;
	for (size_t i = 0; i < n; i++) {
		sqlite3_reset(del);
		sqlite3_bind_int64(del, 1, gone[i]);
		ret = sqlite3_step(del);
		if (ret != SQLITE_DONE) {
			perror_sqlite(ret, "deleting missing file");
			dbfile_commit_trans(db->db);
			goto out;
		}
	}
	sqlite3_reset(del);
	if (dbfile_commit_trans(db->db))
		goto out;

	removed = (int64_t)n;
out:
	if (sel)
		sqlite3_finalize(sel);
	free(gone);
	return removed;
}
