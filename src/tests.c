#include <stdbool.h>
#include <sys/stat.h>

#include "minunit.h"
#include "proptest.h"
#include "rbtree.c"

#include "opt.c"
#include "util.c"
#include "debug.c"
#include "interrupt.c"
#include "csum.c"
#include "threads.c"
#include "btrfs-util.c"
#include "file_scan.c"
#include "filerec.c"
#include "dbfile.c"
#include "hash-tree.c"
#include "results-tree.c"
#include "list_sort.c"
#include "find_dupes.c"
#include "memstats.c"
#include "fiemap.c"
#include "progress.c"
#include "storage.c"
#include "longpath.c"
#include "glob.c"
#include "dedupe.c"


unsigned int blocksize = DEFAULT_BLOCKSIZE;
static char *exec_path;

MU_TEST(test_is_block_zeroed) {
	blocksize = 100;
	char block[100] = {0,};
	// Actual zeroed block
	mu_check(is_block_zeroed(&block) == true);

	// Block has the same content, but not zeroed
	memset(block, 1, 100);
	mu_check(is_block_zeroed(&block) == false);

	// Block do not have the same content
	block[50] = 50;
	mu_check(is_block_zeroed(NULL) == false);
}

MU_TEST(test_block_len) {
	struct file_block block;
	struct filerec file;

	block.b_file = &file;

	// First block of the file
	file.size = 10 * 1024 * 1024;
	block.b_loff = 0;
	mu_check(block_len(&block) == blocksize);

	// block in the middle of the file, unaligned
	block.b_loff = 1;
	mu_check(block_len(&block) == blocksize);

	// block in the middle of the file, aligned
	block.b_loff = blocksize * 10;
	mu_check(block_len(&block) == blocksize);

	// block at the end of the file, which is aligned
	file.size = blocksize * 10;
	block.b_loff = blocksize * 9;
	mu_check(block_len(&block) == blocksize);

	// block at the end of the file, which is unaligned
	unsigned int extra = 10;
	file.size = blocksize * 10 + extra;
	block.b_loff = blocksize * 10;
	mu_check(block_len(&block) == extra);

	// loff is passed filesize
	file.size = blocksize * 10 + extra;
	block.b_loff = blocksize * 15;
	mu_check(block_len(&block) == 0);
}

MU_TEST(test_is_file_renamed) {
	char *new_path = "/tmp/somefile";
	char *path_in_db = "/tmp/somefile";

	mu_check(is_file_renamed(path_in_db, new_path) == false);

	path_in_db = "/tmp/anotherfile";
	mu_check(is_file_renamed(path_in_db, new_path) == true);

	/*
	 * Diffents path but the old one still exists.
	 * We use our own file to simulate a hard link
	 */
	mu_check(is_file_renamed(exec_path, new_path) == false);
}

MU_TEST(test_seen_inode) {
	/*
	 * The scan skips a dirent whose (ino, subvol) was already written this
	 * scan (a further hardlink to one inode), which is how the batched
	 * writer avoids re-storing - and corrupting - a pending filerec. The
	 * match must be exact on both fields: a hash collision that reported a
	 * distinct inode as "seen" would silently drop a real file.
	 */
	seen_inodes_init();
	mu_check(seen_slots != NULL);

	mu_check(seen_inode(42, 7) == false);
	mark_inode_seen(42, 7);
	mu_check(seen_inode(42, 7) == true);	/* the hardlink is skipped */

	/* Same ino in another subvol, or another ino here, is a different file
	 * and must not be reported as seen. */
	mu_check(seen_inode(42, 8) == false);
	mu_check(seen_inode(43, 7) == false);

	/* Values whose 64-bit fields are swapped must not alias each other. */
	mark_inode_seen(7, 42);
	mu_check(seen_inode(7, 42) == true);
	mu_check(seen_inode(42, 7) == true);

	/* Stress the grow/rehash path (initial capacity is 1024): insert many
	 * distinct keys, then verify exact membership survives the resizes. */
	for (uint64_t n = 0; n < 5000; n++)
		mark_inode_seen(1000 + n, n & 3);
	for (uint64_t n = 0; n < 5000; n++)
		mu_check(seen_inode(1000 + n, n & 3) == true);
	mu_check(seen_inode(1000 + 5000, 0) == false);	/* never inserted */
	mu_check(seen_inode(999, 0) == false);

	seen_inodes_free();
}

/*
 * Every bounded writer here has the same contract, so it is stated once: the
 * result is a NUL-terminated prefix of the untruncated form, complete whenever
 * it fits, and nothing outside the caller's size is touched. The fences are the
 * half a length assertion cannot make - `strlen(out) < sz` says nothing about
 * the byte after it, and these are the only callers that can run out of room.
 */
struct fenced { char before[8]; char out[32]; char after[8]; };

#define FENCE_BYTE '#'

static bool fence_intact(const struct fenced *f)
{
	for (size_t i = 0; i < sizeof(f->before); i++)
		if (f->before[i] != FENCE_BYTE || f->after[i] != FENCE_BYTE)
			return false;
	return true;
}

/* True if `f->out` is the prefix of `full` that `sz` bytes can hold. */
static bool wrote_prefix_within(const struct fenced *f, size_t sz, const char *full)
{
	if (sz == 0)
		return f->out[0] == FENCE_BYTE;	/* not even a NUL */
	if (strlen(f->out) >= sz || strncmp(f->out, full, strlen(f->out)))
		return false;
	/* Truncated only when it had to be. */
	return strlen(full) >= sz || !strcmp(f->out, full);
}

/* One fiemap record: {logical, physical, length, flags}. */
struct fm_rec { uint64_t log, phys, len; uint32_t flags; };

static struct fiemap *mkmap(const struct fm_rec *recs, unsigned int n)
{
	struct fiemap *fm = calloc(1, sizeof(*fm) +
				   n * sizeof(struct fiemap_extent));

	fm->fm_mapped_extents = n;
	for (unsigned int i = 0; i < n; i++) {
		fm->fm_extents[i].fe_logical = recs[i].log;
		fm->fm_extents[i].fe_physical = recs[i].phys;
		fm->fm_extents[i].fe_length = recs[i].len;
		fm->fm_extents[i].fe_flags = recs[i].flags;
	}
	return fm;
}

MU_TEST(test_get_extent) {
	/* Three data extents with holes between them:
	 * [0, 4k)   hole   [8k, 12k)   hole   [16k, 20k) */
	struct fm_rec recs[] = {
		{0, 0, 4096, 0}, {8192, 0, 4096, 0}, {16384, 0, 4096, 0}
	};
	struct fiemap *fm = mkmap(recs, ARRAY_SIZE(recs));

	/* Plain lookups (no cursor). */
	mu_check(get_extent(fm, 0, NULL) == &fm->fm_extents[0]);
	mu_check(get_extent(fm, 4095, NULL) == &fm->fm_extents[0]);
	mu_check(get_extent(fm, 4096, NULL) == &fm->fm_extents[1]); /* in hole -> next */
	mu_check(get_extent(fm, 8192, NULL) == &fm->fm_extents[1]);
	mu_check(get_extent(fm, 16384, NULL) == &fm->fm_extents[2]);
	mu_check(get_extent(fm, 20480, NULL) == NULL);             /* past EOF */

	/* A resume cursor must give identical answers for a monotonically
	 * increasing sequence of offsets (the scan access pattern). */
	unsigned int cur = 0;
	size_t offs[] = { 0, 4095, 4096, 8192, 12000, 16384, 19000 };
	for (unsigned int i = 0; i < ARRAY_SIZE(offs); i++)
		mu_check(get_extent(fm, offs[i], &cur) ==
			 get_extent(fm, offs[i], NULL));

	/* A stale cursor pointing past the target must still be correct
	 * (get_extent falls back to a full scan). */
	cur = 2;
	mu_check(get_extent(fm, 0, &cur) == &fm->fm_extents[0]);
	cur = 2;
	mu_check(get_extent(fm, 8192, &cur) == &fm->fm_extents[1]);

	/* Cursor already on the answer while loff sits in the hole just before
	 * it (the sparse scan resuming after a skipped hole): must resolve to
	 * that same extent, so the O(1) resume holds instead of rescanning. */
	cur = 1;
	mu_check(get_extent(fm, 4096, &cur) == &fm->fm_extents[1]);
	mu_check(cur == 1);

	free(fm);
}

/* Build both maps, compare, free. Keeps each case below to its records. */
static bool share(const struct fm_rec *ra, unsigned int na, uint64_t off_a,
		  const struct fm_rec *rb, unsigned int nb, uint64_t off_b,
		  uint64_t len)
{
	struct fiemap *a = mkmap(ra, na), *b = mkmap(rb, nb);
	bool shared = fiemap_maps_share(a, off_a, b, off_b, len);

	free(a);
	free(b);
	return shared;
}

/*
 * fiemap_maps_share() decides whether deduping one range against another would
 * be a no-op. It has to answer "yes" for storage that is genuinely shared but
 * described with different record boundaries, or the same file is resubmitted
 * to the kernel on every run (#186) - and "no" whenever it cannot prove it.
 */
/* Same records, same size -> same key; anything that changes the bytes doesn't. */
static bool key_of(const struct fm_rec *recs, unsigned int n, uint64_t size,
		   unsigned char *out)
{
	struct fiemap *fm = mkmap(recs, n);
	bool ok = fiemap_layout_key(fm, size, out);

	free(fm);
	return ok;
}

MU_TEST(test_fiemap_layout_key) {
	const uint32_t SH = FIEMAP_EXTENT_SHARED;
	const uint32_t ENC = FIEMAP_EXTENT_ENCODED;
	unsigned char a[DIGEST_LEN], b[DIGEST_LEN];

	struct fm_rec two[] = {{0, 4096, 8192, 0}, {8192, 65536, 4096, 0}};

	mu_check(key_of(two, 2, 12288, a));

	/* The same layout, described identically, keys the same. */
	mu_check(key_of(two, 2, 12288, b));
	mu_check(memcmp(a, b, DIGEST_LEN) == 0);

	/* SHARED is a refcount property - deduping an unrelated file must not
	 * change what this file's content is. Same for the positional LAST. */
	struct fm_rec shared[] = {{0, 4096, 8192, SH},
				  {8192, 65536, 4096, SH | FIEMAP_EXTENT_LAST}};

	mu_check(key_of(shared, 2, 12288, b));
	mu_check(memcmp(a, b, DIGEST_LEN) == 0);

	/* Different storage, same shape: different key. */
	struct fm_rec moved[] = {{0, 4096, 8192, 0}, {8192, 69632, 4096, 0}};

	mu_check(key_of(moved, 2, 12288, b));
	mu_check(memcmp(a, b, DIGEST_LEN) != 0);

	/* Same records, different file size - a trailing hole fiemap never
	 * reports, and content the digest does cover. */
	mu_check(key_of(two, 2, 16384, b));
	mu_check(memcmp(a, b, DIGEST_LEN) != 0);

	/* A prefix of the same records must not collide with the whole. */
	mu_check(key_of(two, 1, 12288, b));
	mu_check(memcmp(a, b, DIGEST_LEN) != 0);

	/*
	 * Both sides have to sit the same distance into their record, and that
	 * has to be checked *separately* from the address rather than folded
	 * into `phys + offset` arithmetic. On a compressed extent fe_physical
	 * names the extent as a whole, so an offset into it means nothing - and
	 * the two spellings differ exactly here, where the sums coincide and
	 * the addresses do not. Believing this skips a dedupe that was real.
	 *
	 * The target record starts at the range, the destination's starts 4 KiB
	 * before it; 8192+0 and 4096+4096 are the same number and the stored
	 * extents are not the same extent.
	 */
	struct fm_rec at_start[] = {{4096, 8192, 8192, SH}};
	struct fm_rec offset_in[] = {{0, 4096, 8192, SH}};

	mu_check(!share(at_start, 1, 4096, offset_in, 1, 4096, 4096));

	/* Compressed extents are fine: the address is only ever compared. */
	struct fm_rec enc[] = {{0, 4096, 8192, ENC}};

	mu_check(key_of(enc, 1, 8192, b));

	/* Records whose address means nothing, or nothing stable, are refused. */
	struct fm_rec inl[] = {{0, 0, 512, FIEMAP_EXTENT_DATA_INLINE}};
	struct fm_rec delalloc[] = {{0, 0, 8192, FIEMAP_EXTENT_DELALLOC}};
	struct fm_rec unknown[] = {{0, 0, 8192, FIEMAP_EXTENT_UNKNOWN}};
	struct fm_rec crypt[] = {{0, 4096, 8192, FIEMAP_EXTENT_DATA_ENCRYPTED}};

	mu_check(!key_of(inl, 1, 512, b));
	mu_check(!key_of(delalloc, 1, 8192, b));
	mu_check(!key_of(unknown, 1, 8192, b));
	mu_check(!key_of(crypt, 1, 8192, b));

	/* One bad record poisons the whole file, not just itself. */
	struct fm_rec mixed[] = {{0, 4096, 8192, 0},
				 {8192, 0, 4096, FIEMAP_EXTENT_DELALLOC}};

	mu_check(!key_of(mixed, 2, 12288, b));

	/* A file with no extents at all has no layout to speak of. */
	struct fiemap *empty = mkmap(two, 0);

	mu_check(!fiemap_layout_key(empty, 0, b));
	free(empty);
}

MU_TEST(test_fiemap_maps_share) {
	const uint32_t SH = FIEMAP_EXTENT_SHARED;
	const uint32_t ENC = FIEMAP_EXTENT_ENCODED;

	/* Identical single records over the whole range. */
	struct fm_rec one[] = {{0, 4096, 8192, SH}};

	mu_check(share(one, 1, 0, one, 1, 0, 8192));

	/* Different stored extent: not shared. */
	struct fm_rec elsewhere[] = {{0, 8192, 8192, SH}};

	mu_check(!share(one, 1, 0, elsewhere, 1, 0, 8192));

	/*
	 * The regression: same storage, but the destination's tail is split at
	 * the block boundary a previous dedupe stopped on.
	 */
	struct fm_rec whole[] = {{0, 4096, 12288, SH | ENC}};
	struct fm_rec split[] = {{0, 4096, 8192, SH | ENC},
				 {8192, 99999, 4096, ENC}};

	mu_check(share(whole, 1, 0, split, 2, 0, 8192));
	/* ... but not once the range reaches into the split-off part. */
	mu_check(!share(whole, 1, 0, split, 2, 0, 12288));

	/*
	 * Matching holes are shared: a sparse cache file is mostly hole, so the
	 * map simply stops before the end of the range.
	 */
	mu_check(share(one, 1, 0, one, 1, 0, 262144));

	/* A hole facing data is a real difference. */
	struct fm_rec then_data[] = {{0, 4096, 8192, SH}, {8192, 8192, 4096, 0}};

	mu_check(!share(one, 1, 0, then_data, 2, 0, 12288));

	/* Interior holes must line up on both sides. */
	struct fm_rec gapped[] = {{0, 4096, 4096, SH}, {8192, 8192, 4096, SH}};
	struct fm_rec packed[] = {{0, 4096, 4096, SH}, {4096, 8192, 4096, SH}};

	mu_check(share(gapped, 2, 0, gapped, 2, 0, 12288));
	mu_check(!share(gapped, 2, 0, packed, 2, 0, 12288));

	/* No stable physical location: never shared, whatever the offsets say. */
	struct fm_rec delalloc[] = {{0, 0, 8192, FIEMAP_EXTENT_DELALLOC}};

	mu_check(!share(delalloc, 1, 0, delalloc, 1, 0, 8192));

	/*
	 * Ranges at different logical offsets, on the same stored extent at the
	 * same offset into it (the extent pass compares mid-file ranges).
	 */
	struct fm_rec at64k[] = {{65536, 4096, 8192, SH}};
	struct fm_rec at128k[] = {{131072, 4096, 8192, SH}};

	mu_check(share(at64k, 1, 65536, at128k, 1, 131072, 8192));

	/*
	 * A record that begins before the range: shared only when both sides
	 * start the same distance into the same stored extent.
	 */
	struct fm_rec big[] = {{0, 4096, 16384, SH}};
	struct fm_rec offset[] = {{4096, 4096, 12288, SH}};

	mu_check(share(big, 1, 8192, big, 1, 8192, 8192));
	mu_check(!share(big, 1, 8192, offset, 1, 8192, 4096));
}

/* Sort the target's addresses, measure the destination against them, free. */
static uint64_t unshared(const struct fm_rec *rt, unsigned int nt,
			 const struct fm_rec *rd, unsigned int nd,
			 uint64_t dest_off, uint64_t len)
{
	struct fiemap *t = mkmap(rt, nt), *d = nd ? mkmap(rd, nd) : NULL;
	struct fiemap_phys_set seen;
	uint64_t bytes;

	fiemap_phys_set_init(&seen, t);
	bytes = fiemap_unshared_bytes(&seen, d, dest_off, len);
	fiemap_phys_set_free(&seen);
	free(t);
	free(d);
	return bytes;
}

/*
 * fiemap_unshared_bytes() answers "how much would deduping this destination
 * actually stop duplicating" - the figure a run reports as reclaimed. The
 * kernel's own byte count cannot answer it: FIDEDUPERANGE reports the whole
 * compared length even for a range that already shared the target's storage
 * (#187).
 */
MU_TEST(test_fiemap_unshared_bytes) {
	const uint32_t SH = FIEMAP_EXTENT_SHARED;
	struct fm_rec tgt[] = {{0, 4096, 8192, SH}};

	/* Already on the target's extent: nothing would be freed. */
	mu_check(unshared(tgt, 1, tgt, 1, 0, 8192) == 0);

	/* A copy of its own: all of it. */
	struct fm_rec other[] = {{0, 99999, 8192, 0}};

	mu_check(unshared(tgt, 1, other, 1, 0, 8192) == 8192);

	/*
	 * The case the reported figure got wrong: half the destination already
	 * sits on the target's extent, so only the other half is duplicated.
	 */
	struct fm_rec half[] = {{0, 4096, 4096, SH}, {4096, 99999, 4096, 0}};

	mu_check(unshared(tgt, 1, half, 2, 0, 4096 * 2) == 4096);

	/* Position is irrelevant: the same stored extent frees nothing wherever
	 * the destination references it. */
	struct fm_rec elsewhere[] = {{0, 4096, 4096, SH}};

	mu_check(unshared(tgt, 1, elsewhere, 1, 0, 4096) == 0);

	/* Records are clipped to the range, not counted whole. */
	struct fm_rec wide[] = {{0, 99999, 1 << 20, 0}};

	mu_check(unshared(tgt, 1, wide, 1, 0, 8192) == 8192);

	/* A hole frees nothing - there is nothing there to stop duplicating. */
	struct fm_rec late[] = {{65536, 99999, 4096, 0}};

	mu_check(unshared(tgt, 1, late, 1, 0, 8192) == 0);

	/* No usable address on either side: cannot prove anything is shared, so
	 * report it all as duplicated rather than over-claiming a saving. */
	struct fm_rec delalloc[] = {{0, 0, 8192, FIEMAP_EXTENT_DELALLOC}};

	mu_check(unshared(tgt, 1, delalloc, 1, 0, 8192) == 8192);
	mu_check(unshared(delalloc, 1, tgt, 1, 0, 8192) == 8192);

	/* No destination map at all (fiemap failed): same fallback. */
	mu_check(unshared(tgt, 1, NULL, 0, 0, 8192) == 8192);

	/*
	 * The two measures gate different things - one a skip, one a number -
	 * but they must agree at the boundary: anything fiemap_maps_share()
	 * calls fully shared has nothing left to free. Shared fixtures, so a
	 * future relaxation of one cannot silently drift from the other.
	 */
	struct fm_rec tail[] = {{0, 4096, 8192, SH}, {8192, 99999, 4096, 0}};

	/* Identical maps. */
	mu_check(share(tgt, 1, 0, tgt, 1, 0, 8192));
	mu_check(unshared(tgt, 1, tgt, 1, 0, 8192) == 0);
	/* A tail split off past the compared range. */
	mu_check(share(tgt, 1, 0, tail, 2, 0, 8192));
	mu_check(unshared(tgt, 1, tail, 2, 0, 8192) == 0);
	/* Matching holes to the end of the range. */
	mu_check(share(tgt, 1, 0, tgt, 1, 0, 262144));
	mu_check(unshared(tgt, 1, tgt, 1, 0, 262144) == 0);
}

/*
 * Storage two destinations of one group already share with *each other* must
 * be credited once, not once each: releasing that one extent frees its length
 * once (#191). The set the measure carries is what makes that work, so drive
 * it across several destinations the way a group does.
 */
MU_TEST(test_fiemap_unshared_bytes_accumulates) {
	const uint32_t SH = FIEMAP_EXTENT_SHARED;
	struct fm_rec tgt[] = {{0, 4096, 8192, SH}};
	struct fm_rec on_b[] = {{0, 50000, 8192, SH}};
	struct fm_rec on_c[] = {{0, 90000, 8192, 0}};
	struct fiemap *t = mkmap(tgt, 1), *b = mkmap(on_b, 1), *c = mkmap(on_c, 1);
	struct fiemap_phys_set seen;

	fiemap_phys_set_init(&seen, t);

	/* First destination on extent B: its length is genuinely duplicated. */
	mu_check(fiemap_unshared_bytes(&seen, b, 0, 8192) == 8192);
	/* A second destination on the same extent frees nothing further. */
	mu_check(fiemap_unshared_bytes(&seen, b, 0, 8192) == 0);
	/* A third, on storage of its own, is credited again. */
	mu_check(fiemap_unshared_bytes(&seen, c, 0, 8192) == 8192);
	mu_check(fiemap_unshared_bytes(&seen, c, 0, 8192) == 0);
	/* The target's own storage was never creditable. */
	mu_check(fiemap_unshared_bytes(&seen, t, 0, 8192) == 0);

	/*
	 * A record with no usable address must be credited *every* time, not
	 * remembered after the first. fe_physical is zero-or-meaningless there,
	 * so remembering it would make one unwritten extent stand in for every
	 * later one - and the figure this feeds is a saving, where guessing high
	 * is the way to be wrong that a user cannot check.
	 */
	struct fm_rec pending[] = {{0, 0, 8192, FIEMAP_EXTENT_DELALLOC}};
	struct fiemap *d = mkmap(pending, 1);

	mu_check(fiemap_unshared_bytes(&seen, d, 0, 8192) == 8192);
	mu_check(fiemap_unshared_bytes(&seen, d, 0, 8192) == 8192);
	free(d);

	fiemap_phys_set_free(&seen);
	free(t);
	free(b);
	free(c);
}

/* The set has to stay sorted and unique across many merges, or bsearch starts
 * missing addresses and the over-count creeps back in one destination at a
 * time. Drive enough destinations to force it through several growths. */
MU_TEST(test_fiemap_phys_set_grows) {
	struct fiemap *t = mkmap((struct fm_rec[]){{0, 4096, 4096, 0}}, 1);
	struct fiemap_phys_set seen;

	fiemap_phys_set_init(&seen, t);
	free(t);

	for (unsigned int i = 0; i < 200; i++) {
		/* Descending addresses, so every merge prepends. */
		struct fm_rec r[] = {{0, 1000000 - i * 4096, 4096, 0}};
		struct fiemap *d = mkmap(r, 1);

		mu_check(fiemap_unshared_bytes(&seen, d, 0, 4096) == 4096);
		mu_check(fiemap_unshared_bytes(&seen, d, 0, 4096) == 0);
		free(d);
	}

	mu_check(seen.n == 201);		/* target + 200 distinct */
	for (unsigned int i = 1; i < seen.n; i++)
		mu_check(seen.v[i - 1] < seen.v[i]);	/* sorted, unique */

	fiemap_phys_set_free(&seen);
}

MU_TEST(test_sanitize_ctrl) {
	char out[64];

	/* Plain ASCII and legitimate multi-byte UTF-8 pass through unchanged. */
	sanitize_ctrl("plain.txt", out, sizeof(out));
	mu_check(strcmp(out, "plain.txt") == 0);
	sanitize_ctrl("café-Β.txt", out, sizeof(out));   /* é=C3A9, Β=CE92 */
	mu_check(strcmp(out, "café-Β.txt") == 0);

	/* Whitespace controls keep their familiar spelling... */
	sanitize_ctrl("a\tb\nc\rd", out, sizeof(out));
	mu_check(strcmp(out, "a\\tb\\nc\\rd") == 0);

	/* ... every other C0 control, and DEL, is named by its byte. */
	sanitize_ctrl("esc\x1b[2Jx\x07\x7f", out, sizeof(out));
	mu_check(strcmp(out, "esc\\x1b[2Jx\\x07\\x7f") == 0);

	/* C1 control U+009F (UTF-8 C2 9F): named by its code point, not by
	 * either of the two bytes that spell it (#353). */
	sanitize_ctrl("Te\xc2\x9ft", out, sizeof(out));
	mu_check(strcmp(out, "Te\\x9ft") == 0);

	/* Truncation stays NUL-terminated and within bounds, and never splits an
	 * escape: "ab" + a 4-byte escape does not fit in 6, so it stops at "ab". */
	char small[4];
	sanitize_ctrl("abcdef", small, sizeof(small));
	mu_check(strcmp(small, "abc") == 0);
	char six[6];
	sanitize_ctrl("ab\x1b" "cd", six, sizeof(six));
	mu_check(strcmp(six, "ab") == 0);

	/* ctrl_seq_len() is the one classifier; has_ctrl() the fast path out. */
	unsigned char cp = 0;

	mu_check(ctrl_seq_len((const unsigned char *)"a", &cp) == 0);
	mu_check(ctrl_seq_len((const unsigned char *)"\x1b", &cp) == 1 && cp == 0x1b);
	mu_check(ctrl_seq_len((const unsigned char *)"\x7f", &cp) == 1 && cp == 0x7f);
	/*
	 * A C1 costs two input bytes and is named by its code point - at *both*
	 * ends of the range and not just the top. Only U+009F was checked here
	 * before, which left `>= 0x80` and `<= 0x9f` each satisfied by one
	 * example: a sweep turned `>= 0x80` into `> 0x80` and `<= 0x9f` into
	 * `== 0x9f` and nothing went red, so U+0080 was reaching the terminal.
	 */
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\x80", &cp) == 2 && cp == 0x80);
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\x8f", &cp) == 2 && cp == 0x8f);
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\x9f", &cp) == 2 && cp == 0x9f);
	/* One below and one above the range, which are ordinary UTF-8. */
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\x7f", &cp) == 0);
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\xa0", &cp) == 0);
	/* 0xc2 not followed by a continuation byte is ordinary UTF-8 lead. */
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\xa9", &cp) == 0);
	/* And the whole range survives the escaper, not only its endpoints. */
	for (unsigned int c1 = 0x80; c1 <= 0x9f; c1++) {
		char name[8], want[8];

		snprintf(name, sizeof(name), "x\xc2%c", (char)c1);
		snprintf(want, sizeof(want), "x\\x%02x", c1);
		sanitize_ctrl(name, out, sizeof(out));
		mu_check(!strcmp(out, want));
	}

	mu_check(!has_ctrl("plain.txt"));
	mu_check(!has_ctrl("café-Β.txt"));
	mu_check(!has_ctrl(""));
	mu_check(has_ctrl("a\rb"));
	mu_check(has_ctrl("Te\xc2\x9ft"));

	/* path_for_display() always has room for the whole escaped path. */
	char *dup = path_for_display("a\x1b" "b\x7f");
	mu_check(dup && strcmp(dup, "a\\x1bb\\x7f") == 0);
	free(dup);
	dup = path_for_display("");
	mu_check(dup && strcmp(dup, "") == 0);
	free(dup);
	dup = path_for_display("café-Β.txt");	/* the fast path: unchanged */
	mu_check(dup && strcmp(dup, "café-Β.txt") == 0);
	free(dup);

	/*
	 * A name that is *entirely* control bytes, where the allocation is
	 * exactly SANITIZE_CTRL_MAX per byte plus the NUL and there is no slack
	 * to absorb an arithmetic slip. A mixed name has room to spare, so the
	 * cases above pass with the `+ 1` removed - and what ships then is a
	 * path silently one character short, which names a different file.
	 */
	dup = path_for_display("\x1b\x1b\x1b");
	mu_check(dup && strcmp(dup, "\\x1b\\x1b\\x1b") == 0);
	free(dup);
}

MU_TEST(test_progress_copy_path) {
	char buf[32];

	/* Fits: copied verbatim. */
	progress_copy_path(buf, sizeof(buf), "abc");
	mu_check(strcmp(buf, "abc") == 0);

	/* Exactly filling the buffer (len == cap) must still elide. */
	progress_copy_path(buf, 4, "abcd");
	mu_check(strlen(buf) < 4);

	/*
	 * Too long: elided with the renderer's single "…", keeping the real
	 * head and the basename so both ends stay readable.
	 */
	progress_copy_path(buf, sizeof(buf),
			   "/a/very/long/directory/path/basename.txt");
	mu_check(strlen(buf) < sizeof(buf));
	mu_check(strstr(buf, "…") != NULL);
	mu_check(buf[0] == '/');                       /* real head kept */
	mu_check(strstr(buf, "name.txt") != NULL);     /* basename kept */

	/* Degenerate caps must stay in bounds and NUL-terminated. */
	for (size_t cap = 1; cap <= sizeof(buf); cap++) {
		memset(buf, 'X', sizeof(buf));
		progress_copy_path(buf, cap, "/some/quite/long/path/name.txt");
		mu_check(strlen(buf) < cap);
		mu_check(buf[cap - 1] == '\0' || buf[cap - 1] == 'X');
	}

	/* cap 0 writes nothing at all. */
	memset(buf, 'X', sizeof(buf));
	progress_copy_path(buf, 0, "abcdef");
	mu_check(buf[0] == 'X');
}

/*
 * A path is shortened twice on its way to the screen: once into the worker
 * slot's fixed buffer, then again to the terminal width. Both stages use
 * ellipsize_path(), so the drawn line carries exactly one "…" -- not one
 * marker per stage -- and still shows the real head and the real basename.
 */
MU_TEST(test_progress_path_two_stage_render) {
	char deep[9000];
	char slot[PATH_MAX + 1];
	char drawn[PATH_MAX + 4];
	size_t n = 0;
	const char *p;
	int markers = 0;

	n += snprintf(deep + n, sizeof(deep) - n, "/head-marker");
	while (n < sizeof(deep) - 300)
		n += snprintf(deep + n, sizeof(deep) - n, "/%0*d", 200, 7);
	snprintf(deep + n, sizeof(deep) - n, "/basename.txt");
	mu_check(strlen(deep) > PATH_MAX);

	progress_copy_path(slot, sizeof(slot), deep);
	ellipsize_path(slot, drawn, sizeof(drawn), 100);

	for (p = drawn; (p = strstr(p, "…")); p += strlen("…"))
		markers++;
	mu_check(markers == 1);
	mu_check(strstr(drawn, "/head-marker") == drawn);
	mu_check(strstr(drawn, "basename.txt") != NULL);
}

MU_TEST(test_storage_recommend_io_threads) {
	struct storage_profile p;

	/* SSD / non-rotational: keep the full CPU-capped default (cap 8). */
	p = (struct storage_profile){ .rotational = false,
		.rotational_known = true, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 4) == 4);
	mu_check(storage_recommend_io_threads(&p, 32) == 8);	/* capped */

	/* Unknown media falls back to the same default, never fewer. */
	p = (struct storage_profile){ .rotational = false,
		.rotational_known = false, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 16) == 8);
	mu_check(storage_recommend_io_threads(&p, 2) == 2);

	/* Single spinning disk: few concurrent readers (seek-bound), max 4. */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = true, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 32) == 4);
	mu_check(storage_recommend_io_threads(&p, 2) == 2);	/* fewer cores wins */

	/* HDD pool: ~2 readers per spindle, still capped at 8 and by cores. */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = true, .num_devices = 2 };
	mu_check(storage_recommend_io_threads(&p, 32) == 4);
	p.num_devices = 4;
	mu_check(storage_recommend_io_threads(&p, 32) == 8);	/* 2*4, capped 8 */
	mu_check(storage_recommend_io_threads(&p, 6) == 6);	/* cores limit */

	/* Degenerate CPU count still yields at least one thread. */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = true, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 0) == 1);
}

MU_TEST(test_scan_bucket) {
	/* Boundaries: <1 MiB => 0, then one bucket per power of two above 1 MiB. */
	mu_check(scan_bucket(0) == 0);
	mu_check(scan_bucket(1) == 0);
	mu_check(scan_bucket((1u << 20) - 1) == 0);		/* just under 1 MiB */
	mu_check(scan_bucket(1u << 20) == 1);			/* 1 MiB (2^20) */
	mu_check(scan_bucket((1u << 21) - 1) == 1);		/* just under 2 MiB */
	mu_check(scan_bucket(1u << 21) == 2);			/* 2 MiB */
	mu_check(scan_bucket(1u << 22) == 3);			/* 4 MiB */
	mu_check(scan_bucket(1u << 24) == 5);			/* 16 MiB */
	mu_check(scan_bucket(100ull << 20) == 7);		/* 100 MiB (2^26 top bit) */
	mu_check(scan_bucket(8ull << 30) == 14);		/* 8 GiB (2^33) */
	/* Even the largest possible size stays a valid bucket index. */
	mu_check(scan_bucket(~0ull) == 44);			/* 2^63 top bit */
	mu_check(scan_bucket(~0ull) < SCAN_NBUCKETS);
}

MU_TEST(test_scan_workq_priority) {
	/*
	 * A free thread must take the largest-bucket work first, FIFO (walk order)
	 * within a bucket. Drive scan_workq_push/pop directly (no workers): with
	 * items queued, pop() returns immediately in dispatch order.
	 */
	memset(&scan_workq, 0, sizeof(scan_workq));

	struct file_to_scan files[] = {
		{ .filesize = 512u << 10, .file_position = 1 },	/* 512 KiB -> b0 */
		{ .filesize = 8u << 20,   .file_position = 2 },	/* 8 MiB   -> b4 */
		{ .filesize = 2u << 20,   .file_position = 3 },	/* 2 MiB   -> b2 */
		{ .filesize = 8u << 20,   .file_position = 4 },	/* 8 MiB   -> b4 */
		{ .filesize = 100u << 20, .file_position = 5 },	/* 100 MiB -> b7 */
	};
	for (unsigned int i = 0; i < G_N_ELEMENTS(files); i++)
		scan_workq_push(&files[i]);

	/* Biggest bucket first; within b4, FIFO keeps pos2 before pos4. */
	struct file_to_scan *f;
	f = scan_workq_pop(&scan_workq); mu_check(f->file_position == 5);	/* b7 */
	f = scan_workq_pop(&scan_workq); mu_check(f->file_position == 2);	/* b4 */
	f = scan_workq_pop(&scan_workq); mu_check(f->file_position == 4);	/* b4 */
	f = scan_workq_pop(&scan_workq); mu_check(f->file_position == 3);	/* b2 */
	f = scan_workq_pop(&scan_workq); mu_check(f->file_position == 1);	/* b0 */

	/* Empty + draining => pop returns NULL (worker would exit). */
	scan_workq.draining = true;
	mu_check(scan_workq_pop(&scan_workq) == NULL);

	memset(&scan_workq, 0, sizeof(scan_workq));
}

static gpointer pop_one(gpointer arg)
{
	struct file_to_scan **got = arg;

	*got = scan_workq_pop(&scan_workq);
	return NULL;
}

MU_TEST(test_starved_worker_line_reads_idle) {
	/*
	 * A csum worker holds its display line across files, so the status the
	 * last file left behind ("commit") is what a starved queue would keep
	 * showing - for the whole rest of a walk-bound run. The worker publishes
	 * how long it has been waiting for its next file and the renderer draws
	 * a long enough wait as idle; a short one (the gap between two small
	 * files) must still show the file's real status, or the line flickers.
	 */
	memset(&scan_workq, 0, sizeof(scan_workq));

	struct file_to_scan file = { .filesize = 4096, .file_position = 1 };
	struct file_to_scan *got = NULL;
	struct pscan_thread *slot = pscan_claim_slot(4242, thread_committing);

	/* Waiting on an empty queue: the worker blocks in the pop. */
	GThread *popper = g_thread_new("pop", pop_one, &got);

	pscan_slot_waiting(slot, true);
	mu_check(!slot_is_idle(slot));			/* just started waiting */
	slot->waiting_since -= IDLE_AFTER_US + 1;	/* ... a while ago */
	mu_check(slot_is_idle(slot));

	/* Still claimed while it waits, so no sibling takes over its line. */
	mu_check(pscan_claim_slot(4243, thread_scanning) != slot);

	/* Back to work: the line shows the file again, not idle. */
	scan_workq_push(&file);
	g_thread_join(popper);
	pscan_slot_waiting(slot, false);
	mu_check(got == &file);
	mu_check(!slot_is_idle(slot));

	pscan_free_threads();
	memset(&scan_workq, 0, sizeof(scan_workq));
}

/* Within eps of expected. */
static bool near(double got, double want, double eps)
{
	double e = got - want;
	return e < eps && e > -eps;
}

MU_TEST(test_scan_eta) {
	const uint64_t GiB = 1ull << 30, W = 1ull << 30;   /* 1 GiB per-file weight */

	/* Weighted progress: work = bytes + W*files, ETA = elapsed*(total-done)/done. */

	/* Nothing scanned yet -> no estimate. */
	mu_check(scan_eta_seconds(0, 0, 4 * GiB, 0, W, 10.0) < 0.0);

	/* Pure files (weight is what counts): 1 of 4 files done in 10 s -> 30 s left.
	 * done_work = W, total_work = 4*W, eta = 10*(4-1)/1. */
	mu_check(near(scan_eta_seconds(0, 1, 0, 4, W, 10.0), 30.0, 1e-6));

	/* Pure bytes: 2 of 8 GiB in 12 s -> 36 s. */
	mu_check(near(scan_eta_seconds(2 * GiB, 0, 8 * GiB, 0, W, 12.0), 36.0, 1e-6));

	/* Mixed, weight ties them together: done_work = 1 GiB + W*1 = 2 GiB,
	 * total_work = 1 GiB + W*5 = 6 GiB, eta = 10*(6-2)/2 = 20 s. */
	mu_check(near(scan_eta_seconds(GiB, 1, GiB, 5, W, 10.0), 20.0, 1e-6));

	/* A larger weight up-weights the remaining files, raising the estimate:
	 * done_work = 1 GiB + 2 GiB*1 = 3 GiB, total = 1 GiB + 2 GiB*5 = 11 GiB,
	 * eta = 10*(11-3)/3. */
	mu_check(near(scan_eta_seconds(GiB, 1, GiB, 5, 2 * GiB, 10.0),
		      10.0 * 8.0 / 3.0, 1e-6));

	/* Done >= total -> 0, never negative or a fallback signal. */
	mu_check(near(scan_eta_seconds(4 * GiB, 4, 4 * GiB, 4, W, 10.0), 0.0, 1e-6));
}

MU_TEST(test_group_u64) {
	char b[28];

	/* Under 1000: unchanged. */
	group_u64_snprintf(0, b, sizeof(b));
	mu_check(strcmp(b, "0") == 0);
	group_u64_snprintf(7, b, sizeof(b));
	mu_check(strcmp(b, "7") == 0);
	group_u64_snprintf(999, b, sizeof(b));
	mu_check(strcmp(b, "999") == 0);

	/* Separators every three digits from the right. */
	group_u64_snprintf(1000, b, sizeof(b));
	mu_check(strcmp(b, "1,000") == 0);
	group_u64_snprintf(12000, b, sizeof(b));
	mu_check(strcmp(b, "12,000") == 0);
	group_u64_snprintf(2505166, b, sizeof(b));
	mu_check(strcmp(b, "2,505,166") == 0);

	/* Full width UINT64_MAX still fits the 28-byte buffer. */
	group_u64_snprintf(18446744073709551615ull, b, sizeof(b));
	mu_check(strcmp(b, "18,446,744,073,709,551,615") == 0);

	/*
	 * Truncation, asserted on the bytes rather than on the length. `strlen
	 * <= 3` is satisfied by almost any arithmetic slip in the three
	 * `str_bytes - 1` bounds, which is why sixteen mutants across those
	 * three lines survived it.
	 */
	char small[4];
	group_u64_snprintf(2505166, small, sizeof(small));
	mu_check(!strcmp(small, "2,5"));		/* "2,505,166" cut to fit */
	char one[2];
	group_u64_snprintf(2505166, one, sizeof(one));
	mu_check(!strcmp(one, "2"));
	char just_a_nul[1];
	group_u64_snprintf(2505166, just_a_nul, sizeof(just_a_nul));
	mu_check(just_a_nul[0] == '\0');
	/* The return value is what was written, not what was wanted. */
	mu_check(group_u64_snprintf(2505166, small, sizeof(small)) == 3);
	mu_check(group_u64_snprintf(999, b, sizeof(b)) == 3);
}

/*
 * Whatever the number and whatever the room, the result is a NUL-terminated
 * prefix of the full grouping, the return value is its length, and nothing is
 * written past the buffer. The three `str_bytes - 1` bounds are the whole of
 * this function's difficulty and a table cannot walk every cut point.
 */
MU_TEST(test_prop_group_u64_truncates_to_a_prefix) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		struct fenced fenced;
		char full[32];
		/* Log-uniform, so every digit width comes up. */
		uint64_t n = prop_u64(&p) >> prop_below(&p, 64);
		size_t sz = (size_t)prop_below(&p, sizeof(fenced.out) + 1);
		int ret;

		group_u64_snprintf(n, full, sizeof(full));
		memset(&fenced, FENCE_BYTE, sizeof(fenced));
		ret = group_u64_snprintf(n, fenced.out, sz);

		prop_check(&p, fence_intact(&fenced));
		prop_check(&p, wrote_prefix_within(&fenced, sz, full));
		/* And the one part that is this function's own: it reports what it
		 * wrote, not what it wanted to. */
		prop_check(&p, (size_t)ret == (sz ? strlen(fenced.out) : 0));
	}
}

/*
 * longpath: reach a file whose absolute path exceeds PATH_MAX. Builds a chain
 * of 255-char directories (via incremental chdir, since the leaf's own path is
 * too long to pass to a syscall) under a /tmp temp dir, then checks that
 * longpath_open/longpath_stat reach the deep leaf that a plain open/stat could
 * not. Runs on tmpfs (no reflink needed); best-effort teardown climbs back out.
 */
#define LP_COMP_LEN 255
/* absdir runs to ~PATH_MAX + 383; a leaf adds at most two more
 * NAME_MAX components on top of that. */
#define LP_PATH_BUF	6144

static void lp_fill(char *buf, char c, int n)
{
	memset(buf, c, n);
	buf[n] = '\0';
}

/*
 * Build the deep tree. *out_levels counts the directories created so far and is
 * updated as we descend, so a failure partway still leaves the caller enough
 * state to tear the tree down (we are chdir'd into it and cannot name it with
 * an absolute path). base_out is emptied first for the same reason.
 */
static int lp_make_deep(char *absdir, size_t abscap, char *base_out,
			size_t base_cap, int *out_levels, const char *victim,
			const char *contents)
{
	char comp[LP_COMP_LEN + 1];
	char base[] = "/tmp/oans-longpath-XXXXXX";
	size_t len = strlen(base);
	int fd;

	lp_fill(comp, 'd', LP_COMP_LEN);
	base_out[0] = '\0';
	*out_levels = 0;

	if (!mkdtemp(base) || len + 1 > base_cap || len + 1 > abscap)
		return -1;
	memcpy(base_out, base, len + 1);
	if (chdir(base) != 0)
		return -1;
	memcpy(absdir, base, len + 1);

	/* Descend until the directory path alone exceeds PATH_MAX, so the walk
	 * exercises the multi-chunk openat chain (not just one openat). */
	while (len < (size_t)PATH_MAX + 128) {
		if (mkdir(comp, 0700) != 0 || chdir(comp) != 0)
			return -1;
		(*out_levels)++;
		if (len + 1 + LP_COMP_LEN + 1 > abscap)
			return -1;
		absdir[len++] = '/';
		memcpy(absdir + len, comp, LP_COMP_LEN + 1);
		len += LP_COMP_LEN;
	}

	fd = open(victim, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	if (write(fd, contents, strlen(contents)) != (ssize_t)strlen(contents)) {
		close(fd);
		return -1;
	}
	close(fd);

	return 0;
}

static void lp_destroy_deep(int savedcwd, const char *base, const char *victim,
			    int levels)
{
	char comp[LP_COMP_LEN + 1];
	int i;

	lp_fill(comp, 'd', LP_COMP_LEN);

	/* CWD is the leaf dir: drop the file, then climb + rmdir each level. */
	unlink(victim);
	for (i = 0; i < levels; i++) {
		if (chdir("..") != 0)
			break;
		rmdir(comp);
	}
	if (savedcwd >= 0 && fchdir(savedcwd) != 0)
		return;
	if (base[0])
		rmdir(base);
}

/*
 * The assertions, split out of test_longpath() below: mu_check() returns from
 * its enclosing function on failure, so keeping them here means a failure can
 * never skip the teardown -- which would otherwise strand the process CWD
 * inside a directory too deep to name and leak the tree under /tmp.
 */
static void lp_check_helpers(const char *absdir, const char *base,
			     const char *victim, const char *contents)
{
	char leaf[LP_PATH_BUF];
	struct stat st;
	int fd, bfd, n;
	char buf[128] = { 0 };
	ssize_t r;
	DIR *d;
	struct dirent *de;
	bool found = false;

	/* The directory itself is past PATH_MAX, forcing the chunked walk. */
	mu_check(strlen(absdir) > PATH_MAX);

	n = snprintf(leaf, sizeof(leaf), "%s/%s", absdir, victim);
	mu_check(n > 0 && (size_t)n < sizeof(leaf));
	mu_check(strlen(leaf) > PATH_MAX);

	/* 1. open + read the deep file. */
	fd = longpath_open(leaf, O_RDONLY);
	mu_check(fd >= 0);
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	mu_check(r == (ssize_t)strlen(contents));
	mu_check(strcmp(buf, contents) == 0);

	/* 2. stat + lstat the deep file. */
	mu_check(longpath_stat(leaf, &st) == 0);
	mu_check((size_t)st.st_size == strlen(contents));
	memset(&st, 0, sizeof(st));
	mu_check(longpath_lstat(leaf, &st) == 0);
	mu_check((size_t)st.st_size == strlen(contents));

	/* 3. opendir the deep directory and list it. */
	d = longpath_opendir(absdir);
	mu_check(d != NULL);
	while ((de = readdir(d))) {
		if (strcmp(de->d_name, victim) == 0) {
			found = true;
			break;
		}
	}
	closedir(d);
	mu_check(found);

	/* 4a. missing final component → ENOENT, not ENAMETOOLONG. */
	{
		char missname[LP_COMP_LEN + 1];
		char miss_leaf[LP_PATH_BUF];

		lp_fill(missname, 'x', LP_COMP_LEN);
		snprintf(miss_leaf, sizeof(miss_leaf), "%s/%s", absdir, missname);
		mu_check(strlen(miss_leaf) > PATH_MAX);
		errno = 0;
		mu_check(longpath_open(miss_leaf, O_RDONLY) < 0);
		mu_check(errno == ENOENT);
		errno = 0;
		mu_check(longpath_stat(miss_leaf, &st) < 0);
		mu_check(errno == ENOENT);
	}

	/* 4b. missing intermediate directory → ENOENT from the ancestor walk. */
	{
		char missdir[LP_COMP_LEN + 1];
		char miss_mid[LP_PATH_BUF];

		lp_fill(missdir, 'z', LP_COMP_LEN);
		snprintf(miss_mid, sizeof(miss_mid), "%s/%s/%s", absdir,
			 missdir, victim);
		mu_check(strlen(miss_mid) > PATH_MAX);
		errno = 0;
		mu_check(longpath_open(miss_mid, O_RDONLY) < 0);
		mu_check(errno == ENOENT);
	}

	/* 5. short path: identical to plain open(). */
	bfd = longpath_open(base, O_RDONLY | O_DIRECTORY);
	mu_check(bfd >= 0);
	close(bfd);
}

/*
 * --- the option parser and the human-readable formatters (util.c) ---
 *
 * These had no unit test at all, which the mutation sweep found rather than
 * anyone noticing: 113 of src/util.c's 203 surviving mutants were in these four
 * functions, against 5-9% survival in the escaping code next door. That is not
 * a weak test, it is an absent one, and the shape of what it lets through is
 * the worrying part - `parse_size` is a ladder of fallthroughs, so dropping one
 * `mult *= 1024` makes `--max-filesize=10G` mean ten megabytes, silently, on a
 * run that otherwise looks exactly right.
 */

/* parse_size takes a mutable string; the option parser hands it argv. */
static uint64_t size_of(const char *s)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%s", s);
	return parse_size(buf);
}

MU_TEST(test_parse_size) {
	/* Bare numbers are bytes. */
	mu_check(size_of("0") == 0);
	mu_check(size_of("1") == 1);
	mu_check(size_of("4096") == 4096);

	/* The ladder, one rung at a time. Every one of these is a separate
	 * fallthrough, and the compiler will not miss one for you. */
	mu_check(size_of("1b") == 1);
	mu_check(size_of("1k") == 1024ULL);
	mu_check(size_of("1m") == 1024ULL * 1024);
	mu_check(size_of("1g") == 1024ULL * 1024 * 1024);
	mu_check(size_of("1t") == 1024ULL * 1024 * 1024 * 1024);
	mu_check(size_of("1p") == 1024ULL * 1024 * 1024 * 1024 * 1024);
	mu_check(size_of("1e") == 1024ULL * 1024 * 1024 * 1024 * 1024 * 1024);

	/* Case is not significant. The switch spells some rungs twice and
	 * leans on tolower() for the rest, so this is not free. */
	mu_check(size_of("2K") == size_of("2k"));
	mu_check(size_of("2M") == size_of("2m"));
	mu_check(size_of("2G") == size_of("2g"));
	mu_check(size_of("2T") == size_of("2t"));
	mu_check(size_of("2P") == size_of("2p"));
	mu_check(size_of("2E") == size_of("2e"));

	/* The multiplier applies to the whole number, not the first digit. */
	mu_check(size_of("123k") == 123ULL * 1024);
	mu_check(size_of("1024k") == size_of("1m"));

	/*
	 * The error paths - an empty value, an unknown descriptor, a suffix
	 * longer than one character - are not exercised here: parse_size()
	 * calls exit() on each, which would take the whole suite with it.
	 * They are covered end-to-end in tests/integration/test_min_filesize.py.
	 */
}

/*
 * Every rung of that ladder, against the arithmetic it stands for. The table
 * above names the rungs; this says the multiplier is exactly 1024 per rung for
 * any value, which is what a mutated `*= 1024` breaks in a way one example
 * might happen to miss.
 */
MU_TEST(test_prop_parse_size_scales_by_the_suffix) {
	declare_prop(p, 20000);
	static const char rungs[] = "bkmgtpe";

	while (prop_next(&p)) {
		unsigned int level = (unsigned int)prop_below(&p, sizeof(rungs) - 1);
		/* Bounded so that the largest rung cannot overflow: 1024^6 is
		 * 2^60, leaving four bits of headroom. */
		uint64_t n = prop_below(&p, 16);
		uint64_t expect = n;
		char buf[32];

		for (unsigned int i = 0; i < level; i++)
			expect *= 1024;

		snprintf(buf, sizeof(buf), "%" PRIu64 "%c", n, rungs[level]);
		prop_check(&p, parse_size(buf) == expect);

		snprintf(buf, sizeof(buf), "%" PRIu64 "%c", n,
			 (char)toupper(rungs[level]));
		prop_check(&p, parse_size(buf) == expect);
	}
}

MU_TEST(test_human_size) {
	char buf[32];

	/* Below a kibibyte the exact byte count is printed, with no decimal -
	 * "0.0 B" for an empty file would be worse than useless. */
	human_size_snprintf(0, buf, sizeof(buf));
	mu_check(!strcmp(buf, "0 B"));
	human_size_snprintf(1023, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1023 B"));

	/* The boundary in both directions. */
	human_size_snprintf(1024, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.0 KiB"));
	human_size_snprintf(1536, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.5 KiB"));

	human_size_snprintf(1ULL << 20, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.0 MiB"));
	human_size_snprintf(1ULL << 30, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.0 GiB"));
	human_size_snprintf(1ULL << 40, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.0 TiB"));
	human_size_snprintf(1ULL << 50, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.0 PiB"));

	/* The top rung has to stop there: the loop is bounded by the size of
	 * the units array, and one rung further reads past the end of it. */
	human_size_snprintf(1ULL << 60, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.0 EiB"));
	human_size_snprintf(UINT64_MAX, buf, sizeof(buf));
	mu_check(!strcmp(buf, "16.0 EiB"));

	/*
	 * One byte under a mebibyte reads as "1024.0 KiB" rather than
	 * "1.0 MiB": the loop stops on the computed value, which is 1023.999,
	 * and `%.1f` rounds it up afterwards. Cosmetic, and pinned here so
	 * that it is a decision rather than a surprise - a property test
	 * looking for a mantissa below 1024.0 finds this within a few thousand
	 * cases.
	 */
	human_size_snprintf((1ULL << 20) - 1, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1024.0 KiB"));

	/* A caller with no room gets nothing written and is told so. */
	mu_check(human_size_snprintf(1024, buf, 0) == 0);
}

/*
 * Whatever the size, the unit is one of the seven the array holds and the
 * mantissa is in range for it. A mutated loop bound is an out-of-bounds read
 * of `units` - which a plain build prints as whatever followed the array.
 */
MU_TEST(test_prop_human_size_picks_a_real_unit) {
	declare_prop(p, 20000);
	static const char * const units[] = { "B", "KiB", "MiB", "GiB",
					      "TiB", "PiB", "EiB" };

	while (prop_next(&p)) {
		char buf[32], unit[8];
		double v;
		uint64_t size;
		bool known = false;

		/* Log-uniform, so every rung is reached about equally often;
		 * a uniform draw over uint64 is a byte count above a
		 * pebibyte essentially every time. */
		size = prop_u64(&p) >> prop_below(&p, 64);

		prop_check(&p, human_size_snprintf(size, buf, sizeof(buf)) > 0);
		prop_check(&p, sscanf(buf, "%lf %7s", &v, unit) == 2);
		for (unsigned int i = 0; i < ARRAY_SIZE(units); i++)
			known = known || !strcmp(unit, units[i]);
		prop_check(&p, known);
		/* Bytes are printed whole and unscaled; anything else was
		 * divided down until it was under a kibibyte.
		 *
		 * The upper bound is 1024.0 and not just below it, because the
		 * division stops on the *computed* value and `%.1f` then
		 * rounds: 1 MiB - 1 divides to 1023.999 and prints as
		 * "1024.0 KiB". Pinned as a case in test_human_size rather
		 * than tightened away - it is cosmetic, and where the unit
		 * boundary sits is not a testing commit's to move. */
		if (!strcmp(unit, "B"))
			prop_check(&p, v == (double)size && size < 1024);
		else
			prop_check(&p, v >= 1.0 && v <= 1024.0);
	}
}

MU_TEST(test_human_duration) {
	char buf[32];

	human_duration_snprintf(0, buf, sizeof(buf));
	mu_check(!strcmp(buf, "0s"));
	human_duration_snprintf(59, buf, sizeof(buf));
	mu_check(!strcmp(buf, "59s"));

	/* Seconds are zero-padded once minutes appear, so the field does not
	 * jump width as a scan runs - the progress line is redrawn in place. */
	human_duration_snprintf(60, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1m00s"));
	human_duration_snprintf(61, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1m01s"));
	human_duration_snprintf(3599, buf, sizeof(buf));
	mu_check(!strcmp(buf, "59m59s"));

	/* Past an hour the seconds are dropped, not the minutes. */
	human_duration_snprintf(3600, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1h00m"));
	human_duration_snprintf(3660, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1h01m"));
	human_duration_snprintf(86399, buf, sizeof(buf));
	mu_check(!strcmp(buf, "23h59m"));

	/* Rounded to the nearest second rather than truncated, so an ETA of
	 * 0.6s does not read as "0s" for the whole of its last second. */
	human_duration_snprintf(0.6, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1s"));
	human_duration_snprintf(59.5, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1m00s"));

	mu_check(human_duration_snprintf(1, buf, 0) == 0);
}

/*
 * The rendering has to be readable *back*: whatever the duration, parsing the
 * string returns the same number of seconds, to the resolution that form
 * carries. That catches the swaps a table of examples reads straight past - a
 * `/ 60` against a `% 60`, or minutes and seconds the wrong way round, both of
 * which are right for some of the examples anyone would think to write.
 */
MU_TEST(test_prop_human_duration_reads_back) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char buf[32];
		unsigned long s = (unsigned long)prop_below(&p, 100UL * 3600);
		unsigned long h, m, sec;

		human_duration_snprintf((double)s, buf, sizeof(buf));

		if (s < 60) {
			prop_check(&p, sscanf(buf, "%lus", &sec) == 1);
			prop_check(&p, sec == s);
		} else if (s < 3600) {
			prop_check(&p, sscanf(buf, "%lum%lus", &m, &sec) == 2);
			prop_check(&p, sec < 60);
			prop_check(&p, m * 60 + sec == s);
		} else {
			prop_check(&p, sscanf(buf, "%luh%lum", &h, &m) == 2);
			prop_check(&p, m < 60);
			/* Seconds are dropped rather than rounded into the
			 * minute, so what is printed accounts for everything
			 * except them. */
			prop_check(&p, h * 3600 + m * 60 == s - s % 60);
		}
	}
}

MU_TEST(test_num_digits) {
	/* Zero has no digits by this definition, which is what its one caller
	 * wants: a column width for a counter that has not started. */
	mu_check(num_digits(0) == 0);
	mu_check(num_digits(1) == 1);
	mu_check(num_digits(9) == 1);
	mu_check(num_digits(10) == 2);
	mu_check(num_digits(99) == 2);
	mu_check(num_digits(100) == 3);
	mu_check(num_digits(ULLONG_MAX) == 20);
}

/* Against printf, which is the definition anyone actually means by it. */
MU_TEST(test_prop_num_digits_matches_printf) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char buf[32];
		/* Log-uniform: a uniform draw is a twenty-digit number
		 * essentially every time, and every other width is the
		 * interesting one. */
		unsigned long long n = prop_u64(&p) >> prop_below(&p, 64);

		snprintf(buf, sizeof(buf), "%llu", n);
		prop_check(&p, num_digits(n) == (n ? (int)strlen(buf) : 0));
	}
}

MU_TEST(test_longpath) {
	const char *contents = "over-the-PATH_MAX limit\n";
	char victim[LP_COMP_LEN + 1];
	char absdir[LP_PATH_BUF];
	char base[64] = { 0 };
	int levels = 0;
	int savedcwd = open(".", O_PATH | O_CLOEXEC);
	int rc = -1;

	lp_fill(victim, 'v', LP_COMP_LEN);

	/*
	 * Build, check, tear down -- then assert. Teardown must run before any
	 * mu_check() that could return early, and we only descend at all once we
	 * hold an fd we can fchdir() back to.
	 */
	if (savedcwd >= 0) {
		rc = lp_make_deep(absdir, sizeof(absdir), base, sizeof(base),
				  &levels, victim, contents);
		if (rc == 0)
			lp_check_helpers(absdir, base, victim, contents);
		lp_destroy_deep(savedcwd, base, victim, levels);
		close(savedcwd);
	}

	mu_check(savedcwd >= 0);
	mu_check(rc == 0);
}


/* --- gitignore-style --exclude matching (glob.c) --- */

/*
 * Match one path against one pattern. A pattern that fails to compile aborts
 * rather than returning false: laundering it into "no match" would let every
 * negative assertion below pass vacuously.
 */
static bool gs_hit(const char *pattern, const char *path, bool is_dir)
{
	char *err = NULL;
	struct glob_set *gs = glob_set_new();
	bool r;

	if (glob_set_add(gs, pattern, &err) || glob_set_compile(gs, &err)) {
		fprintf(stderr, "glob pattern \"%s\" rejected: %s\n", pattern, err);
		abort();
	}
	r = glob_set_match(gs, path, is_dir, NULL);
	glob_set_free(gs);
	return r;
}

MU_TEST(test_glob_basename) {
	/* The #147 case: a bare name matches at any depth. Under the old
	 * full-path fnmatch these all silently matched nothing. */
	mu_check(gs_hit("@eaDir", "/srv/media/@eaDir", true));
	mu_check(gs_hit("@eaDir", "/srv/a/b/c/@eaDir", true));
	mu_check(gs_hit("node_modules", "/home/u/p/node_modules", true));
	/* ...but only whole components. */
	mu_check(!gs_hit("@eaDir", "/srv/media/@eaDirectory", true));
	mu_check(!gs_hit("@eaDir", "/srv/media/x@eaDir", true));
	mu_check(!gs_hit("node_modules", "/home/u/node_modules_old", true));

	mu_check(gs_hit("*.iso", "/data/img/x.iso", false));
	mu_check(!gs_hit("*.iso", "/data/img/x.iso.part", false));
}

MU_TEST(test_glob_anchored_vs_any_depth) {
	/* Leading '/' anchors at the filesystem root. */
	mu_check(gs_hit("/srv/media/cache*", "/srv/media/cache1", false));
	mu_check(!gs_hit("/srv/media/cache*", "/other/srv/media/cache1", false));

	/* An interior '/' matches at any depth. */
	mu_check(gs_hit("Steam/temp", "/data/Steam/temp", true));
	mu_check(gs_hit("Steam/temp", "/home/u/games/Steam/temp", true));
	mu_check(!gs_hit("Steam/temp", "/data/Steamx/temp", true));
}

MU_TEST(test_glob_wildcards_respect_separators) {
	/* '*' must not cross a '/'. */
	mu_check(gs_hit("/a/*", "/a/b", false));
	mu_check(!gs_hit("/a/*", "/a/b/c", false));

	/* '**' does cross. */
	mu_check(gs_hit("/a/**/t", "/a/t", false));
	mu_check(gs_hit("/a/**/t", "/a/b/t", false));
	mu_check(gs_hit("/a/**/t", "/a/b/c/d/t", false));

	/* '?' is exactly one non-separator. */
	mu_check(gs_hit("a?.txt", "/x/ab.txt", false));
	mu_check(!gs_hit("a?.txt", "/x/abc.txt", false));
	mu_check(!gs_hit("a?.txt", "/x/a/.txt", false));
}

/*
 * `**` not followed by a separator, which is a different branch of
 * glob_to_regex from the `**\/` above and had no test at all - the mutation
 * sweep found nine survivors on those two lines.
 *
 * It is also where this implementation and gitignore disagree, so these cases
 * are as much a record of the disagreement as a check on it. glob.h states two
 * rules that collide - a pattern with no '/' is a basename rule, and `**`
 * crosses directory boundaries - and here the second wins. git resolves it the
 * other way: in gitignore `**` is only special as a whole path component, and
 * inside one it means `*`. Nobody writes `node**` on purpose and the
 * difference only ever excludes more than was asked for, so what is pinned
 * here is what the code does, not what it arguably should.
 */
MU_TEST(test_glob_double_star_without_a_separator) {
	/* Trailing: everything below the named directory, and the directory
	 * itself only if something follows the stars. */
	mu_check(gs_hit("/a/**", "/a/b", false));
	mu_check(gs_hit("/a/**", "/a/b/c/d", false));

	/* Interior, inside one component: the stars cross '/' where a single
	 * '*' would not. This is the divergence from gitignore. */
	mu_check(gs_hit("/a**z", "/ab/cd/z", false));
	mu_check(!gs_hit("/a*z", "/ab/cd/z", false));

	/* And so a bare pattern with '**' stops being purely a basename rule:
	 * it matches through the separator on the left. */
	mu_check(gs_hit("a**", "/a/bbc", false));
	mu_check(!gs_hit("a**", "/bbc", false));

	/* Three or more stars are the same as two - the run is counted, not
	 * matched pairwise. */
	mu_check(gs_hit("/a/***/t", "/a/b/c/t", false));
}

/*
 * A backslash escapes the next character, so a pattern can name a file that
 * has a metacharacter in it. One line of glob_to_regex, no test, and nine
 * surviving mutants on it - including the `i + 1 < len` bound, whose failure
 * is a read one past the end of the pattern.
 */
MU_TEST(test_glob_backslash_escapes) {
	/* An escaped wildcard is a literal, and stops being a wildcard. */
	mu_check(gs_hit("a\\*b", "/x/a*b", false));
	mu_check(!gs_hit("a\\*b", "/x/axxb", false));

	/* Same for the other metacharacters, so a real name gets named. */
	mu_check(gs_hit("a\\?b", "/x/a?b", false));
	mu_check(!gs_hit("a\\?b", "/x/azb", false));
	mu_check(gs_hit("db\\[1].hash", "/var/db[1].hash", false));

	/* The escape consumes exactly one character; what follows is ordinary
	 * again. */
	mu_check(gs_hit("a\\**b", "/x/a*zzb", false));

	/* A trailing backslash has nothing to escape. It must be treated as a
	 * literal rather than reaching past the end of the pattern for a
	 * character that is not there. */
	mu_check(gs_hit("a\\", "/x/a\\", false));
	mu_check(!gs_hit("a\\", "/x/ab", false));
}

MU_TEST(test_glob_character_classes) {
	mu_check(gs_hit("f[0-9].log", "/x/f3.log", false));
	mu_check(!gs_hit("f[0-9].log", "/x/fx.log", false));
	mu_check(gs_hit("f[!0-9].log", "/x/fx.log", false));
	mu_check(!gs_hit("f[!0-9].log", "/x/f3.log", false));
	mu_check(gs_hit("f[abc].log", "/x/fb.log", false));

	/* A '.' in the pattern is a literal, not "any character". */
	mu_check(!gs_hit("a.txt", "/x/axtxt", false));
}

MU_TEST(test_glob_directory_only) {
	/* A trailing '/' restricts the pattern to directories. */
	mu_check(gs_hit("cache/", "/a/cache", true));
	mu_check(!gs_hit("cache/", "/a/cache", false));
	/* Without it, either kind matches. */
	mu_check(gs_hit("cache", "/a/cache", true));
	mu_check(gs_hit("cache", "/a/cache", false));
}

MU_TEST(test_glob_literal_paths_are_not_globs) {
	/* An exact path oans excludes on the user's behalf must match itself
	 * even when it contains regex/glob metacharacters. */
	char *err = NULL;
	struct glob_set *gs = glob_set_new();

	glob_set_add_literal(gs, "/tmp/h[1].db");
	mu_check(glob_set_compile(gs, &err) == 0);
	mu_check(glob_set_match(gs, "/tmp/h[1].db", false, NULL));
	mu_check(!glob_set_match(gs, "/tmp/h1.db", false, NULL));
	glob_set_free(gs);
}

MU_TEST(test_glob_reports_matching_pattern_and_counts) {
	char *err = NULL;
	struct glob_set *gs = glob_set_new();
	const char *which = NULL, *pat = NULL;
	bool matched = false;

	mu_check(glob_set_add(gs, "*.log", &err) == 0);
	mu_check(glob_set_add(gs, "@eaDir", &err) == 0);
	mu_check(glob_set_compile(gs, &err) == 0);

	mu_check(glob_set_match(gs, "/a/b/x.log", false, &which));
	mu_check(which && strcmp(which, "*.log") == 0);

	mu_check(glob_set_match(gs, "/a/@eaDir", true, &which));
	mu_check(which && strcmp(which, "@eaDir") == 0);

	/* Per-pattern flags back the "matched nothing" warning (#147). */
	mu_check(glob_set_stat(gs, 0, &pat, &matched) && matched);
	mu_check(glob_set_stat(gs, 1, &pat, &matched) && matched);
	mu_check(!glob_set_stat(gs, 2, &pat, &matched));
	glob_set_free(gs);
}

MU_TEST(test_glob_rejects_malformed) {
	char *err = NULL;
	struct glob_set *gs = glob_set_new();

	mu_check(glob_set_add(gs, "f[abc", &err) != 0);
	mu_check(err != NULL);
	g_free(err);
	glob_set_free(gs);
}

MU_TEST(test_glob_empty_set_matches_nothing) {
	char *err = NULL;
	struct glob_set *gs = glob_set_new();

	mu_check(glob_set_compile(gs, &err) == 0);
	mu_check(!glob_set_match(gs, "/anything", false, NULL));
	glob_set_free(gs);
}

/*
 * The property the whole of #159 rests on: hashing a byte stream in one go and
 * hashing it across a save/restore must give the same digest. If it ever did
 * not, a resumed scan would store a digest matching nothing - silently, since
 * nothing downstream can tell a wrong digest from a file that simply has no
 * duplicate.
 */
MU_TEST(test_running_checksum_survives_save_restore) {
	unsigned char data[8192];
	unsigned char whole[DIGEST_LEN], resumed[DIGEST_LEN];
	_cleanup_(freep) void *blob = malloc(running_checksum_state_size());
	struct running_checksum *c;

	for (unsigned int i = 0; i < sizeof(data); i++)
		data[i] = (unsigned char)(i * 31 + (i >> 3));

	c = start_running_checksum();
	add_to_running_checksum(c, data, sizeof(data));
	finish_running_checksum(c, whole);

	/*
	 * Split at 1000 bytes: not a multiple of XXH3's 256-byte internal
	 * buffer, so the state carries buffered bytes across the break - the
	 * case a naive "just keep the accumulators" save would get wrong.
	 */
	c = start_running_checksum();
	add_to_running_checksum(c, data, 1000);
	mu_check(running_checksum_save(c, blob, running_checksum_state_size()) == 0);
	finish_running_checksum(c, NULL);

	c = running_checksum_restore(blob, running_checksum_state_size());
	mu_check(c != NULL);
	add_to_running_checksum(c, data + 1000, sizeof(data) - 1000);
	finish_running_checksum(c, resumed);

	mu_check(memcmp(whole, resumed, DIGEST_LEN) == 0);
}

/*
 * A restored state must not follow the pointer it was saved with.
 * XXH3_state_t holds one to the library's static secret, whose address is
 * whatever this process's loader chose - so the saved copy is meaningless in
 * the process that reads it back, and every checkpoint is read back by a
 * different process than wrote it. Nothing in-process can notice: the two
 * addresses are the same one until the blob crosses a process boundary, which
 * is why the saved pointer is scribbled on here rather than trusted to differ.
 */
MU_TEST(test_running_checksum_repoints_the_secret_on_restore) {
	unsigned char data[512];
	unsigned char whole[DIGEST_LEN], resumed[DIGEST_LEN];
	size_t len = running_checksum_state_size();
	_cleanup_(freep) unsigned char *blob = malloc(len);
	struct running_checksum *c;

	for (unsigned int i = 0; i < sizeof(data); i++)
		data[i] = (unsigned char)(i * 7);

	c = start_running_checksum();
	add_to_running_checksum(c, data, sizeof(data));
	finish_running_checksum(c, whole);

	c = start_running_checksum();
	add_to_running_checksum(c, data, 300);
	mu_check(running_checksum_save(c, blob, len) == 0);
	finish_running_checksum(c, NULL);

	/* What another process's copy of the same state looks like from here. */
	memset(blob + sizeof(struct csum_state_hdr) +
	       offsetof(XXH3_state_t, extSecret), 0xa5, sizeof(void *));

	c = running_checksum_restore(blob, len);
	mu_check(c != NULL);
	add_to_running_checksum(c, data + 300, sizeof(data) - 300);
	finish_running_checksum(c, resumed);
	mu_check(memcmp(whole, resumed, DIGEST_LEN) == 0);
}

/*
 * A buffer that cannot hold the state is refused and left alone. The caller is
 * the checkpoint writer, and a short buffer there would otherwise be a write
 * past whatever it had allocated.
 */
MU_TEST(test_running_checksum_save_refuses_a_short_buffer) {
	size_t len = running_checksum_state_size();
	_cleanup_(freep) unsigned char *buf = malloc(len);
	struct running_checksum *c = start_running_checksum();

	memset(buf, 0x5a, len);
	mu_check(running_checksum_save(c, buf, len - 1) != 0);
	for (size_t i = 0; i < len; i++)
		mu_check(buf[i] == 0x5a);	/* nothing was written */

	/* The control: one byte more and it is written in full. */
	mu_check(running_checksum_save(c, buf, len) == 0);
	mu_check(buf[0] != 0x5a || buf[1] != 0x5a);
	finish_running_checksum(c, NULL);
}

/* A blob this binary cannot vouch for must be refused, not reinterpreted. */
MU_TEST(test_running_checksum_rejects_foreign_state) {
	size_t len = running_checksum_state_size();
	_cleanup_(freep) unsigned char *blob = malloc(len);
	struct running_checksum *c = start_running_checksum();

	mu_check(running_checksum_save(c, blob, len) == 0);
	finish_running_checksum(c, NULL);

	/* The control: unmodified, this one has to be accepted. */
	c = running_checksum_restore(blob, len);
	mu_check(c != NULL);
	finish_running_checksum(c, NULL);

	/* A different xxhash - what a distro upgrade leaves behind. */
	blob[8] ^= 0xff;
	mu_check(running_checksum_restore(blob, len) == NULL);
	blob[8] ^= 0xff;

	/* Truncated, or from a build whose state struct was a different size. */
	mu_check(running_checksum_restore(blob, len - 1) == NULL);

	/* Not one of ours at all. */
	blob[0] ^= 0xff;
	mu_check(running_checksum_restore(blob, len) == NULL);
}

/* --- the hashfile (dbfile.c) ---
 *
 * These run against an in-memory SQLite database, which oans already supports:
 * `dbfile_open_handle(NULL)` is the same path a run without `--hashfile` takes,
 * so nothing here is a test-only seam. That is what makes dbfile.c reachable
 * from the unit suite at all - it needs no filesystem, no btrfs, and no scan.
 *
 * What is worth testing here is not "does SQLite work" but the handful of
 * invariants this file has actually broken before, each of which fails
 * *silently*: a hashfile that empties itself while the run exits 0, a resumed
 * file whose stored hashes vanish, a replay that adopts the wrong default.
 */

/*
 * A fresh in-memory hashfile.
 *
 * Nothing is cleared, because there is nothing to clear: a shared-cache
 * in-memory database exists only while a connection is open, so the moment the
 * last handle closes SQLite frees it, tables and all. Every caller below takes
 * its handle with `_cleanup_(sqlite3_close_cleanup)`, which is what makes that
 * true even when an assertion fails partway - `mu_check()` expands to a bare
 * `return`, so a hand-written close is skipped exactly when a test goes red.
 * Isolation by construction rather than by a hand-maintained `delete from`
 * list, which goes stale silently the first time the schema grows a table.
 *
 * Costs one schema build and 30 prepared statements per test, measured at
 * 1.6 ms. That is 5% of the suite and 0.07% of the mutation tool's hang
 * timeout, which is the budget that actually binds - a shared handle would buy
 * the 13 ms back and put isolation on the stale list instead.
 */
static struct dbhandle *memdb(void)
{
	struct dbhandle *db = dbfile_open_handle(NULL);

	if (!db)
		abort();	/* the test cannot run at all, not a finding */
	return db;
}

/*
 * Row count for the tables no shipped API reports. `files`, `blocks` and
 * `extents` deliberately go through dbfile_get_stats() instead - that is what
 * `--stats` and `--json` print, so asserting through it covers the production
 * counters as well as the tables.
 */
/*
 * A statement whose only job is to set the fixture up. It aborts rather than
 * mu_check()s: a mistyped column name makes sqlite3_exec() a no-op, and a
 * fixture that quietly did nothing is a test that quietly proves nothing -
 * which is how the wrong-length-blob case below first passed against a column
 * that does not exist.
 */
static void exec(struct dbhandle *db, const char *sql)
{
	char *err = NULL;

	if (sqlite3_exec(db->db, sql, NULL, NULL, &err) != SQLITE_OK)
		abort();
	sqlite3_free(err);
}
static uint64_t rows(struct dbhandle *db, const char *table)
{
	char sql[64];

	snprintf(sql, sizeof(sql), "select count(*) from %s", table);
	return dbfile_query_u64(db->db, sql);
}

/* The three counts oans itself reports. */
static struct dbfile_stats stats_of(struct dbhandle *db)
{
	struct dbfile_stats st = {0};

	if (dbfile_get_stats(db, &st))
		abort();
	return st;
}

/*
 * A checkpoint carries a real running-checksum state: `file_state` is bound as
 * a blob of running_checksum_state_size() bytes, so a NULL there is a
 * constraint failure rather than an empty column. The caller owns the buffers,
 * which is also how dbfile_load_checkpoint() reads one back - one convention,
 * not two.
 *
 * `ext_state` is the half that matters most (#159): a checkpoint at an extent
 * boundary needs none, but btrfs reports a contiguously allocated file as a
 * single fiemap extent however large it is, so the boundary-only first cut
 * never fired. Pass NULL for the boundary case and a buffer for the in-flight
 * one.
 */
static struct scan_checkpoint mkcheckpoint(void *file_state, void *ext_state,
					   uint64_t loff)
{
	size_t len = running_checksum_state_size();
	struct running_checksum *c = start_running_checksum();
	struct scan_checkpoint cp = {
		.loff = loff, .size = loff * 2, .mtime = 1000,
		.ext_loff = 0, .ext_len = 4096,
		.file_state = file_state,
		.ext_state = ext_state,
		.has_ext_state = ext_state != NULL,
	};

	add_to_running_checksum(c, (unsigned char *)"some bytes", 10);
	if (running_checksum_save(c, file_state, len))
		abort();
	finish_running_checksum(c, NULL);
	if (ext_state)
		memcpy(ext_state, file_state, len);
	return cp;
}

/* Store one file row; returns its id. */
/*
 * A row for a file that has been *seen* but not finished - which is what
 * `dbfile_store_file_info` alone produces, because it deliberately writes no
 * digest. The digest is precisely what tells a scanned file from one an
 * interrupted run left partway, so this is the shape the startup prune is
 * about, and the two helpers are split so a test can ask for either.
 */
/* A digest that differs in every byte for each n, so a comparator reading the
 * wrong end of it still sees a difference. */
static void digest_of(unsigned char *out, unsigned int n)
{
	for (unsigned int i = 0; i < DIGEST_LEN; i++)
		out[i] = (unsigned char)(n * 7 + i * 31);
}
/* The one row builder. Everything below adds columns to it rather than
 * repeating the build - two near-identical copies differing by an inert line
 * is what this replaced. */
static int64_t put_row(struct dbhandle *db, const char *name, uint64_t ino,
		       uint64_t subvol, uint64_t size, unsigned int seq)
{
	_cleanup_(file_cleanup) struct file f = {0};
	int64_t id;

	if (file_set_filename(&f, name))
		abort();
	f.ino = ino;
	f.subvol = subvol;
	f.size = size;
	f.mtime = 1000;
	f.dedupe_seq = seq;
	id = dbfile_store_file_info(db, &f);
	if (id <= 0)
		abort();
	return id;
}
static int64_t put_unscanned_file(struct dbhandle *db, const char *name,
				  uint64_t ino, uint64_t subvol)
{
	return put_row(db, name, ino, subvol, 4096, 1);
}

/* ... and the same row finished, the way a completed hash leaves it. */
static int64_t put_file(struct dbhandle *db, const char *name, uint64_t ino,
			uint64_t subvol)
{
	int64_t id = put_unscanned_file(db, name, ino, subvol);
	unsigned char digest[DIGEST_LEN];

	digest_of(digest, (unsigned int)ino);
	if (dbfile_update_scanned_file(db, id, digest, 0, 1))
		abort();
	return id;
}

/* Build the map, ask, free - the shape share() and key_of() above have. */
static int layout_matches(struct dbhandle *db, int64_t fileid,
			  const struct fm_rec *recs, unsigned int n)
{
	struct fiemap *fm = mkmap(recs, n);
	int ret = dbfile_layout_matches(db, fileid, fm);

	free(fm);
	return ret;
}

/*
 * `files` is unique on the *pair* (ino, subvol), and both halves matter: a
 * subvolume id and an inode number are each 64 bits and two different files can
 * share either one alone. Conflating them is the same trap seen_inode() has, so
 * it is pinned here as a property of the schema.
 *
 * What this deliberately does *not* assert is that a second link destroys the
 * first row. That is true today - storing one is an INSERT OR REPLACE - but it
 * is the hazard, not the contract: the protection is an in-memory seen_inodes
 * set in file_scan.c, pinned end-to-end by tests/integration/test_hardlinks.py.
 * A test demanding the destructive behaviour would go red on the day someone
 * makes the write safe, and read as a regression while the tests that actually
 * guard it stayed green. If that day comes, delete this comment rather than
 * repairing anything.
 */
MU_TEST(test_dbfile_files_are_unique_on_the_inode_pair) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();

	put_file(db, "/tree/a", 42, 1);
	mu_check(stats_of(db).num_files == 1);

	/* A different inode in the same subvolume is a different file... */
	put_file(db, "/tree/c", 43, 1);
	mu_check(stats_of(db).num_files == 2);

	/* ...and so is the same inode number in a different subvolume. */
	put_file(db, "/other/a", 42, 2);
	mu_check(stats_of(db).num_files == 3);

}

/*
 * A file's hashes belong to it: dropping the row takes them with it, via the
 * ON DELETE CASCADE. If that FK is ever lost, pruning a deleted file leaves
 * orphan hashes that match nothing and are never collected.
 *
 * Driven through dbfile_prune_missing_files() rather than dbfile_remove_file(),
 * so it exercises the caller the comment is about. That needs no filesystem:
 * the rule is stat-based, and "/tree/a" genuinely does not exist, which is
 * exactly the ENOENT the prune looks for. It is stat-based rather than
 * "delete what this run did not walk" precisely so that scanning a subdirectory
 * cannot wipe rows that are merely out of scope.
 */
MU_TEST(test_dbfile_pruning_a_deleted_file_takes_its_hashes) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t id = put_file(db, "/tree/a", 1, 1);
	struct block_csum blocks[2] = { { .loff = 0 }, { .loff = 4096 } };
	struct extent_csum extents[1] = { { .loff = 0, .poff = 8192, .len = 8192 } };

	mu_check(dbfile_store_block_hashes(db, id, 2, blocks) == 0);
	mu_check(dbfile_store_extent_hashes(db, id, 1, extents) == 0);
	mu_check(stats_of(db).num_b_hashes == 2);
	mu_check(stats_of(db).num_e_hashes == 1);

	mu_check(dbfile_prune_missing_files(db, NULL) == 1);
	mu_check(stats_of(db).num_files == 0);
	mu_check(stats_of(db).num_b_hashes == 0);	/* cascaded */
	mu_check(stats_of(db).num_e_hashes == 0);

}

/*
 * #159's resume path. A file interrupted partway keeps its row, its stored
 * hashes and its checkpoint; the run that picks it up moves it into the current
 * dedupe generation with a *targeted* UPDATE. Re-running the ordinary upsert
 * instead would INSERT OR REPLACE the row and cascade the checkpoint and every
 * stored hash away - the file would silently restart from byte zero, which on
 * the 1 TiB file this feature exists for is hours.
 */
MU_TEST(test_dbfile_advancing_the_generation_keeps_hashes_and_checkpoint) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t id = put_file(db, "/tree/big", 1, 1);
	struct block_csum blocks[1] = { { .loff = 0 } };
	size_t len = running_checksum_state_size();
	_cleanup_(freep) void *file_state = malloc(len);
	_cleanup_(freep) void *ext_state = malloc(len);
	/* Interrupted mid-extent, which is the case that fires in practice:
	 * btrfs reports a contiguously allocated file as one fiemap extent
	 * however large it is, so a boundary-only checkpoint never happens. */
	struct scan_checkpoint cp = mkcheckpoint(file_state, ext_state, 1u << 30);
	/* load_checkpoint copies into buffers the caller owns; a zeroed struct
	 * would be a write through NULL. */
	_cleanup_(freep) void *got_file = malloc(len);
	_cleanup_(freep) void *got_ext = malloc(len);
	struct scan_checkpoint got = { .file_state = got_file, .ext_state = got_ext };

	mu_check(dbfile_store_block_hashes(db, id, 1, blocks) == 0);
	mu_check(dbfile_store_checkpoint(db, id, &cp) == 0);
	mu_check(rows(db, "blocks") == 1);
	mu_check(rows(db, "scan_checkpoints") == 1);

	mu_check(dbfile_update_dedupe_seq(db, id, 7) == 0);

	/* The generation moved... */
	mu_check(dbfile_query_u64(db->db,
		 "select dedupe_seq from files") == 7);
	/* ...and nothing else did. */
	mu_check(rows(db, "files") == 1);
	mu_check(rows(db, "blocks") == 1);
	mu_check(dbfile_load_checkpoint(db, id, &got));
	mu_check(got.loff == cp.loff && got.ext_len == cp.ext_len);
	mu_check(!memcmp(got.file_state, file_state, len));
	/* The in-flight extent digest rides along, or a resume that lands inside
	 * an extent has to discard the checkpoint and start the file over. */
	mu_check(got.has_ext_state);
	mu_check(!memcmp(got.ext_state, ext_state, len));

}

/*
 * The startup prune drops rows that were never hashed - a digest is what says
 * a file was finished - but it has to spare rows an interrupted run left a
 * checkpoint for. Before #159 it deleted every digest-less row, which is
 * exactly what kept a resumable file from surviving to be resumed.
 */
MU_TEST(test_dbfile_pruning_unscanned_files_spares_checkpointed_ones) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	_cleanup_(freep) void *state = malloc(running_checksum_state_size());
	/* No ext_state: a checkpoint that happened to land on an extent
	 * boundary, which is the other half of what dbfile_load_checkpoint
	 * validates. */
	struct scan_checkpoint cp = mkcheckpoint(state, NULL, 4096);

	/* A finished file: has a digest. */
	put_file(db, "/tree/done", 1, 1);

	/* One interrupted with a checkpoint, one simply never hashed. Neither
	 * has a digest, which is the only thing the prune looks at. */
	int64_t pid = put_unscanned_file(db, "/tree/partway", 2, 1);

	put_unscanned_file(db, "/tree/abandoned", 3, 1);

	mu_check(dbfile_store_checkpoint(db, pid, &cp) == 0);
	mu_check(stats_of(db).num_files == 3);

	mu_check(dbfile_prune_unscanned_files(db) == 0);

	/* The finished one and the checkpointed one survive; the third goes. */
	mu_check(stats_of(db).num_files == 2);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from files where filename = '/tree/partway'") == 1);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from files where filename = '/tree/abandoned'") == 0);

}

/*
 * The self-describing hashfile (#182): a 1.7.x file stores
 * opt_skip_readonly_subvols = -1, the old tri-state "auto". That means "the
 * user never asked", so a replay must adopt today's default rather than
 * inheriting a dead one - only an explicit 1 survives. Getting this backwards
 * silently skips every snapshot on a scheduled run.
 */
MU_TEST(test_dbfile_scan_config_round_trips_and_coerces_the_old_auto) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	char *roots[] = { (char *)"/data", (char *)"/srv" };
	char *excludes[] = { (char *)"*.iso" };
	struct scan_config out = {
		.run_dedupe = 1, .recurse = 1, .skip_zeroes = 1,
		.skip_readonly_subvols = 1, .only_whole_files = 1,
		.do_block_hash = 1, .dedupe_same_file = 1,
		.min_filesize = 1024, .max_filesize = 1u << 20,
		.roots = roots, .nroots = 2,
		.excludes = excludes, .nexcludes = 1,
	};
	struct scan_config in = {0};

	/* 0 means "nothing stored", which is what a fresh hashfile answers and
	 * what tells a replay from a first run. */
	mu_check(dbfile_load_scan_config(db, &in) == 0);
	mu_check(in.nroots == 0);

	mu_check(dbfile_store_scan_config(db, &out) == 0);
	mu_check(dbfile_load_scan_config(db, &in) == 1);

	/*
	 * Every option, not a representative few: a replay adopts whatever this
	 * does not read, so an option the loader drops silently reverts to its
	 * default on a scheduled run and the hashfile stops describing what
	 * produced it. All 1 here, so a dropped key reads as 0.
	 */
	mu_check(in.run_dedupe == 1 && in.recurse == 1 && in.do_block_hash == 1);
	mu_check(in.skip_zeroes == 1 && in.only_whole_files == 1);
	mu_check(in.dedupe_same_file == 1);
	mu_check(in.min_filesize == 1024 && in.max_filesize == (1u << 20));
	mu_check(in.skip_readonly_subvols == 1);	/* an explicit 1 survives */
	mu_check(in.nroots == 2 && in.nexcludes == 1);
	mu_check(!strcmp(in.roots[0], "/data") && !strcmp(in.roots[1], "/srv"));
	mu_check(!strcmp(in.excludes[0], "*.iso"));
	scan_config_free(&in);

	/* What a 1.7.x hashfile holds. */
	exec(db, "update config set keyval = -1 "
		 "where keyname = 'opt_skip_readonly_subvols'");
	memset(&in, 0, sizeof(in));
	mu_check(dbfile_load_scan_config(db, &in) == 1);
	mu_check(in.skip_readonly_subvols == 0);	/* coerced to the new default */
	scan_config_free(&in);

	/* ...and the mirror image, so that all-1 above cannot be a constant. */
	memset(&out, 0, sizeof(out));
	out.roots = roots;
	out.nroots = 2;
	out.min_filesize = 4096;
	mu_check(dbfile_store_scan_config(db, &out) == 0);
	memset(&in, 0, sizeof(in));
	mu_check(dbfile_load_scan_config(db, &in) == 1);
	mu_check(!in.run_dedupe && !in.recurse && !in.do_block_hash);
	mu_check(!in.skip_zeroes && !in.only_whole_files && !in.dedupe_same_file);
	scan_config_free(&in);

	/*
	 * A key that is simply not there. Sizes are the two that a hashfile
	 * written by an older oans can lack, and the default has to be "no
	 * limit" rather than whatever the caller's struct happened to hold -
	 * a stale non-zero max_filesize skips every large file for good.
	 */
	exec(db, "delete from config where keyname in "
		 "('opt_min_filesize', 'opt_max_filesize')");
	memset(&in, 0x5a, sizeof(in));
	mu_check(dbfile_load_scan_config(db, &in) == 1);
	mu_check(in.min_filesize == 0 && in.max_filesize == 0);
	mu_check(in.run_dedupe == 0 && in.nroots == 2);
	scan_config_free(&in);

	/*
	 * Only the additive one missing, which is what a hashfile written
	 * before --max-filesize existed actually looks like. The two limits
	 * are read through one scratch variable, so a reset dropped between
	 * them silently gives max_filesize the value of min - and every file
	 * above the lower limit stops being scanned, on a run that otherwise
	 * looks identical.
	 */
	mu_check(dbfile_store_scan_config(db, &out) == 0);
	exec(db, "delete from config where keyname = 'opt_max_filesize'");
	memset(&in, 0x5a, sizeof(in));
	mu_check(dbfile_load_scan_config(db, &in) == 1);
	mu_check(in.min_filesize == 4096);
	mu_check(in.max_filesize == 0);
	scan_config_free(&in);
}

/*
 * Run history, which `--history` and `--json` are read straight out of. The
 * lifetime totals accumulate across runs while the error buckets report only
 * the *last* one - a monitoring consumer alarms on the second and trends on
 * the first, so conflating them either hides a fault or re-raises a fixed one
 * forever.
 */
MU_TEST(test_dbfile_run_history_totals_accumulate_but_skips_are_the_last_run) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct run_record first = {
		.ts = 1000, .duration_ms = 500, .files_scanned = 10,
		.reclaimed = 4096, .groups = 2, .deduped = 1,
		.skip_permission = 3, .skip_unreadable = 1,
	};
	/* Five adjacent columns read back by index, so five distinct values:
	 * two buckets that share one cannot tell a swapped pair apart, and
	 * a bucket left at 0 cannot tell a dropped read from a real zero. */
	struct run_record second = {
		.ts = 2000, .duration_ms = 700, .files_scanned = 5,
		.reclaimed = 8192, .groups = 1, .deduped = 1,
		.skip_permission = 11, .skip_unreadable = 22,
		.skip_path_too_long = 33, .skip_unsupported_fs = 44,
		.readonly_subvols = 55,
	};
	struct run_record third = {
		.ts = 3000, .duration_ms = 100, .files_scanned = 1,
	};
	/* Deliberately not zeroed: the summary clears what it does not fill,
	 * so a caller's stack garbage cannot be read back as a lifetime total. */
	struct run_summary s;

	memset(&s, 0x5a, sizeof(s));

	/*
	 * A hashfile nobody has run against yet. Every field must be zero,
	 * which is what says the summary clears the caller's struct rather
	 * than filling in only what its two queries return - with no rows,
	 * they return nothing at all. (It has to be asked here: memdb()
	 * handles all share one in-memory database, so there is no such thing
	 * as a second, empty one within a test.)
	 */
	mu_check(dbfile_get_run_summary(db, &s) == 0);
	mu_check(s.runs == 0 && s.total_reclaimed == 0 && s.total_files == 0);
	mu_check(s.first_ts == 0 && s.last_ts == 0 && s.total_skip_errors == 0);
	mu_check(s.last_skip_permission == 0 && s.last_skip_unreadable == 0);
	mu_check(s.last_skip_path_too_long == 0);
	mu_check(s.last_skip_unsupported_fs == 0 && s.last_readonly_subvols == 0);

	memset(&s, 0x5a, sizeof(s));
	mu_check(dbfile_record_run(db, &first) == 0);
	mu_check(dbfile_record_run(db, &second) == 0);
	mu_check(dbfile_get_run_summary(db, &s) == 0);

	mu_check(s.runs == 2);
	mu_check(s.total_reclaimed == 4096 + 8192);	/* lifetime */
	mu_check(s.total_files == 15);
	mu_check(s.first_ts == 1000 && s.last_ts == 2000);

	/* Each bucket, from the most recent run only. */
	mu_check(s.last_skip_permission == 11);
	mu_check(s.last_skip_unreadable == 22);
	mu_check(s.last_skip_path_too_long == 33);
	mu_check(s.last_skip_unsupported_fs == 44);
	mu_check(s.last_readonly_subvols == 55);
	/* ...while the lifetime figure sums both runs. readonly_subvols is not
	 * an error, so it is not in the total. */
	mu_check(s.total_skip_errors == 3 + 1 + 11 + 22 + 33 + 44);

	/*
	 * A later clean run clears the alarm. A lifetime total only ever
	 * grows, so it cannot tell "last night lost a subtree" from "one run
	 * did, months ago" - which is the whole reason these are read
	 * separately from the sums above.
	 */
	mu_check(dbfile_record_run(db, &third) == 0);
	mu_check(dbfile_get_run_summary(db, &s) == 0);
	mu_check(s.last_skip_permission == 0 && s.last_skip_unreadable == 0);
	mu_check(s.last_skip_path_too_long == 0);
	mu_check(s.last_skip_unsupported_fs == 0 && s.last_readonly_subvols == 0);
	mu_check(s.total_skip_errors == 3 + 1 + 11 + 22 + 33 + 44);

}

/*
 * #206's second bar. A layout key is a 128-bit digest, so it cannot be the
 * whole test: before copying a donor's hashes the scan re-checks the donor's
 * stored (loff, poff, len) triples one at a time. A miss costs one redundant
 * hash; a false hit stores a digest of bytes the file never had, and nothing
 * downstream could tell that from a file with no duplicate.
 */
MU_TEST(test_dbfile_layout_matches_compares_every_record) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t donor = put_file(db, "/snap/a", 1, 1);
	struct extent_csum stored[2] = {
		{ .loff = 0,    .poff = 4096,  .len = 8192 },
		{ .loff = 8192, .poff = 65536, .len = 4096 },
	};
	struct fm_rec same[]  = { {0, 4096, 8192, 0}, {8192, 65536, 4096, 0} };
	struct fm_rec moved[] = { {0, 4096, 8192, 0}, {8192, 69632, 4096, 0} };
	struct fm_rec shortr[] = { {0, 4096, 8192, 0} };
	struct fm_rec longer[] = { {0, 4096, 8192, 0}, {8192, 65536, 4096, 0},
				   {12288, 73728, 4096, 0} };

	mu_check(dbfile_store_extent_hashes(db, donor, 2, stored) == 0);

	mu_check(layout_matches(db, donor, same, ARRAY_SIZE(same)) == 1);
	/* One address moved: not the same storage. */
	mu_check(layout_matches(db, donor, moved, ARRAY_SIZE(moved)) == 0);
	/* The candidate has fewer records than the donor, and more. Both
	 * directions, because a prefix match is still a miss. */
	mu_check(layout_matches(db, donor, shortr, ARRAY_SIZE(shortr)) == 0);
	mu_check(layout_matches(db, donor, longer, ARRAY_SIZE(longer)) == 0);

	/*
	 * A donor whose rows are gone - an aborted batch, a hardlink cascade -
	 * verifies as a miss by construction rather than needing a guard, which
	 * is why the in-memory map can hold only a key and a file id.
	 */
	int64_t empty = put_file(db, "/snap/b", 2, 1);

	mu_check(layout_matches(db, empty, same, ARRAY_SIZE(same)) == 0);

	/*
	 * And the case that needs saying separately: *both* sides empty. The
	 * record counts then agree at zero, so a check that only compared them
	 * would call two files with no extents a match and copy one's digest
	 * onto the other. "No records" is not evidence of identical storage,
	 * it is the absence of evidence.
	 */
	mu_check(layout_matches(db, empty, same, 0) == 0);

}

/*
 * The copy itself (#206): the destination ends up with the donor's extent rows,
 * block rows and digest. Everything downstream reads those, so a copy that
 * dropped one of the three would leave a file that looks scanned and dedupes
 * against nothing.
 */
MU_TEST(test_dbfile_copying_a_donor_brings_all_three) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t donor = put_file(db, "/snap/a", 1, 1);
	int64_t dst;
	struct block_csum blocks[2] = { { .loff = 0 }, { .loff = 4096 } };
	struct extent_csum extents[1] = { { .loff = 0, .poff = 4096, .len = 8192 } };

	mu_check(dbfile_store_block_hashes(db, donor, 2, blocks) == 0);
	mu_check(dbfile_store_extent_hashes(db, donor, 1, extents) == 0);

	/* The destination starts with no digest, the way a file does before it
	 * has been hashed. */
	dst = put_unscanned_file(db, "/snap/b", 2, 1);

	mu_check(dbfile_copy_scanned_file(db, dst, donor, 0) == 0);

	mu_check(stats_of(db).num_b_hashes == 4);	/* two each */
	mu_check(stats_of(db).num_e_hashes == 2);
	/* And the destination now has a digest, so it reads as scanned. */
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from files where digest is not null") == 2);

}

/*
 * Every field a stored row carries, read back through the change-detection
 * path the scan itself uses. The original test asserted only that a row
 * appeared, which leaves each `sqlite3_bind_*` free to write the right value
 * into the wrong column - a sweep found the bind indices in
 * dbfile_store_file_info() entirely unguarded. Distinct values throughout, so
 * a swapped pair cannot look correct.
 */
MU_TEST(test_dbfile_a_stored_file_round_trips_every_field) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	_cleanup_(file_cleanup) struct file f = {0};
	_cleanup_(file_cleanup) struct file got = {0};
	unsigned char digest[DIGEST_LEN];
	int64_t id;

	if (file_set_filename(&f, "/tree/distinctive-name"))
		abort();
	f.ino = 111; f.subvol = 222; f.size = 333; f.mtime = 444;
	f.dedupe_seq = 5;
	id = dbfile_store_file_info(db, &f);
	mu_check(id > 0);

	/*
	 * Before the digest lands the row exists but is not a finished file -
	 * which is the whole distinction an interrupted run turns on, and the
	 * only thing that tells the two apart.
	 */
	mu_check(dbfile_describe_file(db, 111, 222, &got) == 0);
	mu_check(got.id == id && !got.digest_valid);
	file_cleanup(&got);
	memset(&got, 0, sizeof(got));

	memset(digest, 0xab, DIGEST_LEN);
	mu_check(dbfile_update_scanned_file(db, id, digest, 6, 7) == 0);

	/* Looked up by (ino, subvol), which is how the scan finds it again. */
	mu_check(dbfile_describe_file(db, 111, 222, &got) == 0);
	mu_check(got.id == id);
	mu_check(got.size == 333);
	mu_check(got.mtime == 444);
	mu_check(got.filename && !strcmp(got.filename, "/tree/distinctive-name"));
	mu_check(got.digest_valid);	/* it has been hashed all the way through */

	/* The columns describe_file does not return, and the ones only
	 * update_scanned_file writes. */
	mu_check(dbfile_query_u64(db->db, "select ino from files") == 111);
	mu_check(dbfile_query_u64(db->db, "select subvol from files") == 222);
	mu_check(dbfile_query_u64(db->db, "select dedupe_seq from files") == 5);
	mu_check(dbfile_query_u64(db->db, "select flags from files") == 6);
	mu_check(dbfile_query_u64(db->db, "select nr_extents from files") == 7);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from files "
		 "where hex(digest) = 'ABABABABABABABABABABABABABABABAB'") == 1);

	/* A file nothing knows about leaves the caller's struct alone rather
	 * than reporting someone else's row. */
	_cleanup_(file_cleanup) struct file absent = {0};

	mu_check(dbfile_describe_file(db, 999, 999, &absent) == 0);
	mu_check(absent.id == 0 && absent.size == 0 && absent.filename == NULL);
}

/*
 * Hashes carry the offsets that say where they came from, and the loaders join
 * on them. Asserting only the row *count* - which is what the first cut did -
 * leaves every bind index free: a block hash stored at the wrong loff still
 * counts as one row, and dedupe then compares the wrong parts of two files.
 */
MU_TEST(test_dbfile_hashes_round_trip_their_offsets) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t id = put_unscanned_file(db, "/tree/a", 1, 1);
	struct block_csum blocks[2] = { { .loff = 4096 }, { .loff = 8192 } };
	struct extent_csum extents[2] = {
		{ .loff = 0,    .poff = 65536, .len = 4096 },
		{ .loff = 4096, .poff = 69632, .len = 8192 },
	};

	memset(blocks[0].digest, 0x11, DIGEST_LEN);
	memset(blocks[1].digest, 0x22, DIGEST_LEN);
	memset(extents[0].digest, 0x33, DIGEST_LEN);
	memset(extents[1].digest, 0x44, DIGEST_LEN);

	mu_check(dbfile_store_block_hashes(db, id, 2, blocks) == 0);
	mu_check(dbfile_store_extent_hashes(db, id, 2, extents) == 0);

	mu_check(dbfile_query_u64(db->db,
		 "select loff from blocks order by loff limit 1") == 4096);
	mu_check(dbfile_query_u64(db->db,
		 "select loff from blocks order by loff desc limit 1") == 8192);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from blocks where fileid = (select id from files)") == 2);
	/* Paired with its offset, so a digest bound into the wrong slot shows. */
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from blocks where loff = 4096 and "
		 "hex(digest) = '11111111111111111111111111111111'") == 1);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from blocks where loff = 8192 and "
		 "hex(digest) = '22222222222222222222222222222222'") == 1);

	/* Each extent's three numbers, together: a swapped poff/len pair keeps
	 * both the row count and either column's set of values intact. */
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from extents where loff = 0 and poff = 65536 "
		 "and len = 4096") == 1);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from extents where loff = 4096 and poff = 69632 "
		 "and len = 8192") == 1);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from extents where loff = 0 and "
		 "hex(digest) = '33333333333333333333333333333333'") == 1);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from extents where loff = 4096 and "
		 "hex(digest) = '44444444444444444444444444444444'") == 1);

	/*
	 * A zero-length extent is skipped rather than stored: it names no
	 * bytes, so a later join against it would match a range that does not
	 * exist.
	 */
	struct extent_csum empty[2] = {
		{ .loff = 65536, .poff = 1, .len = 0 },
		{ .loff = 69632, .poff = 2, .len = 1 },
	};

	mu_check(dbfile_store_extent_hashes(db, id, 2, empty) == 0);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from extents where len = 0") == 0);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from extents where len = 1") == 1);
}

/*
 * The checkpoint reader declines everything it cannot vouch for. Reporting
 * success for a checkpoint that is absent or the wrong size hands the scan a
 * resume position built from an uninitialised buffer, and the digest that comes
 * out describes bytes no file ever held.
 */
MU_TEST(test_dbfile_load_checkpoint_declines_what_it_cannot_vouch_for) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t id = put_unscanned_file(db, "/tree/big", 1, 1);
	size_t len = running_checksum_state_size();
	_cleanup_(freep) void *state = malloc(len);
	_cleanup_(freep) void *got_file = malloc(len);
	_cleanup_(freep) void *got_ext = malloc(len);
	struct scan_checkpoint cp = mkcheckpoint(state, NULL, 4096);
	struct scan_checkpoint got = { .file_state = got_file, .ext_state = got_ext };

	/* Distinct throughout: these are five adjacent columns read by index,
	 * so two that share a value cannot tell a swapped pair apart. */
	cp.size = 123456;
	cp.mtime = 777;
	cp.ext_loff = 2048;
	cp.ext_len = 999;

	/* Nothing stored yet: not an error, but not a checkpoint either. */
	mu_check(!dbfile_load_checkpoint(db, id, &got));

	mu_check(dbfile_store_checkpoint(db, id, &cp) == 0);
	mu_check(dbfile_load_checkpoint(db, id, &got));
	mu_check(got.loff == 4096 && got.size == 123456 && got.mtime == 777);
	mu_check(got.ext_loff == 2048 && got.ext_len == 999);
	mu_check(!got.has_ext_state);	/* none was stored, so none comes back */
	/* The saved hash state itself, which is the point of the whole row. */
	mu_check(!memcmp(got_file, state, len));

	/* A file that has one tells nothing about a file that does not. */
	int64_t other = put_unscanned_file(db, "/tree/other", 2, 1);

	mu_check(!dbfile_load_checkpoint(db, other, &got));

	/* A state blob of the wrong length is refused rather than copied out
	 * of - the length is the only thing standing between a truncated row
	 * and a memcpy past the caller's buffer. */
	exec(db, "update scan_checkpoints set state = x'0011'");
	mu_check(!dbfile_load_checkpoint(db, id, &got));

	/* And once removed it is gone, so a finished file cannot resume. */
	mu_check(dbfile_store_checkpoint(db, id, &cp) == 0);
	mu_check(dbfile_load_checkpoint(db, id, &got));
	mu_check(dbfile_remove_checkpoint(db, id) == 0);
	mu_check(!dbfile_load_checkpoint(db, id, &got));
}

/* Which id the seen-oracle below claims the walk confirmed on disk. */
static int64_t seen_oracle_id;

static bool claims_seen(int64_t id)
{
	return id == seen_oracle_id;
}

/*
 * The prune's actual contract, which the first cut only tested from one side:
 * it removes rows whose path is *gone* and keeps everything else. Testing only
 * the removal half leaves "delete what this run did not walk" passing, and that
 * is the mistake the rule exists to prevent - it would wipe a shared hashfile
 * the moment anyone scanned a subdirectory.
 *
 * Needs a real file, but any filesystem will do: the rule is a stat(), not a
 * dedupe.
 */
MU_TEST(test_dbfile_pruning_keeps_what_still_exists) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	char present[] = "/tmp/oans-prune-XXXXXX";
	int fd = mkstemp(present);

	if (fd < 0)
		abort();
	close(fd);

	put_file(db, present, 1, 1);
	put_file(db, "/tmp/oans-prune-definitely-not-here", 2, 1);
	mu_check(stats_of(db).num_files == 2);

	/* One gone, one still there. */
	mu_check(dbfile_prune_missing_files(db, NULL) == 1);
	mu_check(stats_of(db).num_files == 1);
	mu_check(dbfile_query_u64(db->db, "select ino from files") == 1);

	/*
	 * The seen-oracle is the scan's "I confirmed this one on disk" answer,
	 * and it must short-circuit the stat entirely - that is the whole point
	 * of it, since the common case is that nothing was deleted. Claim the
	 * missing file was seen and it survives.
	 */
	int64_t ghost = put_file(db, "/tmp/oans-prune-also-not-here", 3, 1);

	seen_oracle_id = ghost;
	mu_check(dbfile_prune_missing_files(db, claims_seen) == 0);
	mu_check(stats_of(db).num_files == 2);

	/* Withdraw the claim and it goes. */
	seen_oracle_id = 0;
	mu_check(dbfile_prune_missing_files(db, claims_seen) == 1);
	mu_check(stats_of(db).num_files == 1);

	/*
	 * ENOTDIR, the other way a path stops naming a file: a component of it
	 * is now a regular file rather than a directory. It is a separate
	 * errno from ENOENT and the check names both, so a test that only
	 * deletes files leaves half of that condition free.
	 */
	char under[sizeof(present) + 8];

	snprintf(under, sizeof(under), "%s/child", present);
	put_file(db, under, 4, 1);
	mu_check(stats_of(db).num_files == 2);
	mu_check(dbfile_prune_missing_files(db, NULL) == 1);
	mu_check(dbfile_query_u64(db->db, "select ino from files") == 1);

	/*
	 * A stat that failed for any *other* reason is not "gone", and the row
	 * has to survive: EACCES on a directory whose permissions changed, or
	 * EIO on a disk going bad, would otherwise delete the hashes for files
	 * that are still there - so the run that was meant to notice the disk
	 * is failing rebuilds the cache instead. A symlink loop stands in for
	 * that class, because it is the only one of them a test can produce
	 * without being root or breaking hardware.
	 */
	char loop[] = "/tmp/oans-prune-loop-XXXXXX";

	if (!mkdtemp(loop))
		abort();
	rmdir(loop);
	if (symlink(loop, loop))
		abort();
	put_file(db, loop, 5, 1);
	mu_check(stats_of(db).num_files == 2);
	mu_check(dbfile_prune_missing_files(db, NULL) == 0);
	mu_check(stats_of(db).num_files == 2);
	unlink(loop);

	unlink(present);
}

/*
 * More missing files than the collector's initial capacity, so the array has to
 * grow. 512 is where it starts; a growth that loses or duplicates ids deletes
 * the wrong rows, and at 8 files nothing would ever notice.
 */
MU_TEST(test_dbfile_pruning_grows_past_its_initial_capacity) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	const unsigned int n = 600;
	char name[64];

	mu_check(dbfile_begin_trans(db->db) == 0);
	for (unsigned int i = 0; i < n; i++) {
		snprintf(name, sizeof(name), "/tmp/oans-absent-%u", i);
		put_file(db, name, 1000 + i, 1);
	}
	mu_check(dbfile_commit_trans(db->db) == 0);
	mu_check(stats_of(db).num_files == n);

	mu_check(dbfile_prune_missing_files(db, NULL) == (int64_t)n);
	mu_check(stats_of(db).num_files == 0);
}

/*
 * ---------------------------------------------------------------------------
 * Property-based tests. See src/proptest.h for the harness and for why the
 * seed is fixed; `OANS_PROPTEST_SEED=random ./test` goes looking for more.
 *
 * Everything below states a relationship that has to hold for *every* input,
 * rather than an input and its answer. The tests above are not redundant with
 * these and are not replaced by them: a table of cases says what a function is
 * for and reads as documentation, while a property says what must never
 * happen and reaches inputs nobody sits down and writes.
 * ---------------------------------------------------------------------------
 */

/*
 * A name as hostile as anything a scanned tree can contain: mostly ordinary
 * bytes, with C0 controls, DEL and the two-byte UTF-8 C1 encodings salted in
 * at a rate no uniform draw would reach. The 0xc2 lead byte is emitted on its
 * own often enough to land immediately before the terminator, which is the
 * case ctrl_seq_len() has to read one byte past to decide.
 */
static void gen_hostile_name(struct prop *p, char *buf, size_t sz)
{
	size_t len = (size_t)prop_below(p, sz - 1);

	for (size_t i = 0; i < len; i++) {
		unsigned char c;

		switch (prop_below(p, 8)) {
		case 0:				/* a C0 control or DEL */
			c = prop_chance(p, 4) ? 0x7f
					      : (unsigned char)prop_below(p, 0x20);
			break;
		case 1:				/* the lead byte of a C1 */
			c = 0xc2;
			break;
		case 2:				/* a C1 trail, or a stray one */
			c = (unsigned char)prop_range(p, 0x80, 0x9f);
			break;
		case 3:				/* any byte at all */
			c = (unsigned char)prop_u64(p);
			break;
		default:			/* something a name is made of */
			c = (unsigned char)prop_range(p, 'a', 'z');
			break;
		}
		/* A NUL would end the string early and silently shrink every
		 * case that drew one; the interesting truncation is the
		 * buffer's, and that is generated on purpose below. */
		buf[i] = c ? (char)c : 'x';
	}
	buf[len] = '\0';
}

/*
 * The guarantee #202 rests on: whatever bytes are in a file's name, nothing
 * that reaches the terminal can still act on it. Stated over the classifier
 * rather than over a list of characters, so the two cannot drift - adding an
 * encoding to ctrl_seq_len() tightens this test in the same commit.
 */
MU_TEST(test_prop_sanitize_ctrl_leaves_nothing_dangerous) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char in[24], out[SANITIZE_CTRL_MAX * 24 + 1];

		gen_hostile_name(&p, in, sizeof(in));
		sanitize_ctrl(in, out, sizeof(out));
		prop_check(&p, !has_ctrl(out));
		/*
		 * And again in raw bytes, which is not the redundancy it looks
		 * like. `has_ctrl` is `ctrl_seq_len` and so is the escaper, so
		 * a mutation *inside the classifier* leaves the two agreeing
		 * and this property blind to it - measured: narrowing the C1
		 * test to `> 0x80` was caught by nothing here. A property
		 * phrased in terms of the function under test cannot see the
		 * function being wrong, only inconsistent.
		 */
		for (const unsigned char *q = (const unsigned char *)out; *q; q++) {
			prop_check(&p, *q >= 0x20 && *q != 0x7f);
			prop_check(&p, !(q[0] == 0xc2 && q[1] >= 0x80 && q[1] <= 0x9f));
		}
		/*
		 * And within the bound `path_for_display` sizes its allocation
		 * by, which is load-bearing rather than decorative: an encoding
		 * that expanded further would overflow a heap buffer on the
		 * first crafted name. Asserted here rather than in a property
		 * of its own, which would be this one's setup plus a line.
		 */
		prop_check(&p, strlen(out) <= SANITIZE_CTRL_MAX * strlen(in));
	}
}

/*
 * A short buffer must truncate and never split an escape - half of `\xNN` is
 * two characters of something else. Stated as "the short answer is a prefix of
 * the long one", which says that and also that nothing else changes with the
 * buffer size; a rule about the last four bytes would not.
 *
 * The canaries are the other half: this is the only caller that can run out of
 * room, so a fencepost here writes past a heap allocation in production.
 */
MU_TEST(test_prop_sanitize_ctrl_truncates_on_whole_escapes) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char in[24], full[SANITIZE_CTRL_MAX * 24 + 1];
		struct fenced fenced;
		size_t sz = (size_t)prop_below(&p, sizeof(fenced.out) + 1);

		gen_hostile_name(&p, in, sizeof(in));
		memset(&fenced, FENCE_BYTE, sizeof(fenced));
		sanitize_ctrl(in, fenced.out, sz);
		sanitize_ctrl(in, full, sizeof(full));

		prop_check(&p, fence_intact(&fenced));
		prop_check(&p, wrote_prefix_within(&fenced, sz, full));
	}
}

/*
 * Every real name takes the fast path, and it has to be exactly that - a fast
 * path that also *changed* the name would rewrite most of what oans prints.
 */
MU_TEST(test_prop_sanitize_ctrl_is_identity_on_ordinary_names) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char in[24], out[sizeof(in)];
		size_t len = (size_t)prop_below(&p, sizeof(in) - 1);

		for (size_t i = 0; i < len; i++) {
			unsigned char c;

			/* Anything ctrl_seq_len() does not flag, which
			 * includes the high bytes of ordinary UTF-8. */
			do {
				c = (unsigned char)prop_u64(&p);
			} while (!c || c < 0x20 || c == 0x7f || c == 0xc2);
			in[i] = (char)c;
		}
		in[len] = '\0';

		prop_check(&p, !has_ctrl(in));
		sanitize_ctrl(in, out, sizeof(out));
		prop_check(&p, strcmp(out, in) == 0);

		_cleanup_(freep) char *disp = path_for_display(in);

		prop_check(&p, disp && !strcmp(disp, in));
	}
}

/*
 * #159's whole design constraint: the digest a resumed hash produces is the
 * digest of the same bytes, whatever offsets it was interrupted at. The one
 * fixed case above splits at 1000 bytes because that is not a multiple of
 * XXH3's internal buffer; this asks the same question at every offset, and at
 * several of them in a row, which is what a 1 TiB file actually does.
 */
MU_TEST(test_prop_checksum_resumes_at_any_split) {
	declare_prop(p, 2000);
	unsigned char data[4096];
	size_t blob_len = running_checksum_state_size();
	_cleanup_(freep) void *blob = malloc(blob_len);

	/* Filled once: this property is about *where* a hash was interrupted,
	 * not about content, and drawing 4 KiB per case cost ten times the
	 * hashing it was there to exercise. */
	prop_bytes(&p, data, sizeof(data));

	while (prop_next(&p)) {
		unsigned char whole[DIGEST_LEN], resumed[DIGEST_LEN];
		size_t len = (size_t)prop_below(&p, sizeof(data) + 1);
		unsigned int splits = (unsigned int)prop_below(&p, 4);
		struct running_checksum *c;
		size_t at = 0;

		c = start_running_checksum();
		add_to_running_checksum(c, data, len);
		finish_running_checksum(c, whole);

		c = start_running_checksum();
		for (unsigned int s = 0; s < splits; s++) {
			/* Biased to the offsets that are awkward for a
			 * buffered hash: nothing yet, one byte, and either
			 * side of XXH3's 256-byte block. */
			size_t next;

			switch (prop_below(&p, 4)) {
			case 0: next = at; break;
			case 1: next = at + 1; break;
			case 2: next = (at + 256) & ~(size_t)255; break;
			default: next = (size_t)prop_range(&p, at, len); break;
			}
			if (next > len)
				next = len;
			add_to_running_checksum(c, data + at, next - at);
			at = next;

			prop_check(&p, running_checksum_save(c, blob, blob_len) == 0);
			finish_running_checksum(c, NULL);
			c = running_checksum_restore(blob, blob_len);
			prop_check(&p, c != NULL);
		}
		add_to_running_checksum(c, data + at, len - at);
		finish_running_checksum(c, resumed);

		prop_check(&p, memcmp(whole, resumed, DIGEST_LEN) == 0);
	}
}

/*
 * A checkpoint this binary cannot vouch for has to be refused rather than
 * reinterpreted: restoring a state a different xxhash wrote yields a digest of
 * bytes that never existed, and nothing downstream could tell that from a file
 * with no duplicate. Every bit of the header is part of that promise, so every
 * bit is flipped.
 */
MU_TEST(test_prop_checksum_refuses_any_damaged_header) {
	declare_prop(p, 4000);
	size_t blob_len = running_checksum_state_size();
	size_t hdr_len = sizeof(struct csum_state_hdr);
	_cleanup_(freep) unsigned char *blob = malloc(blob_len);

	while (prop_next(&p)) {
		struct running_checksum *c = start_running_checksum();
		size_t byte = (size_t)prop_below(&p, hdr_len);
		unsigned char bit = (unsigned char)(1u << prop_below(&p, 8));
		unsigned char fed[] = "some bytes";

		add_to_running_checksum(c, fed, sizeof(fed));
		prop_check(&p, running_checksum_save(c, blob, blob_len) == 0);
		finish_running_checksum(c, NULL);

		/* The control: untouched, this one has to be accepted, or the
		 * assertion below would hold for a blob that was never valid. */
		c = running_checksum_restore(blob, blob_len);
		prop_check(&p, c != NULL);
		finish_running_checksum(c, NULL);

		blob[byte] ^= bit;
		prop_check(&p, running_checksum_restore(blob, blob_len) == NULL);
		blob[byte] ^= bit;

		/* A length that is not exactly right is refused too - a short
		 * read of a checkpoint row must not be reinterpreted. */
		prop_check(&p, running_checksum_restore(blob, blob_len - 1) == NULL);
		prop_check(&p, running_checksum_restore(blob, blob_len + 1) == NULL);
	}
}

/* Somewhere between "one extent" and "a shredded file", in whole blocks. */
#define PROP_MAX_RECS	5
#define PROP_BLOCK	4096

/*
 * A plausible extent layout: ascending, block-aligned, sometimes with a hole
 * before a record, addresses drawn from a small pool so that two layouts
 * genuinely collide rather than never doing so. NO_PHYS records appear about
 * one map in eight, because "cannot prove it" is a case with its own rules and
 * a generator that never produced one would leave them to the fixed tests.
 */
static unsigned int gen_layout(struct prop *p, struct fm_rec *out)
{
	unsigned int n = (unsigned int)prop_range(p, 1, PROP_MAX_RECS);
	uint64_t log = 0;

	for (unsigned int i = 0; i < n; i++) {
		if (prop_chance(p, 3))			/* a hole before it */
			log += PROP_BLOCK * prop_range(p, 1, 2);
		out[i].log = log;
		out[i].len = PROP_BLOCK * prop_range(p, 1, 3);
		/* A small pool of addresses, offset so that 0 - which fiemap
		 * uses for "nowhere" - is never one of them. */
		out[i].phys = PROP_BLOCK * prop_range(p, 100, 108);
		out[i].flags = prop_bool(p) ? FIEMAP_EXTENT_SHARED : 0;
		if (prop_chance(p, 40))
			out[i].flags |= FIEMAP_EXTENT_DELALLOC;
		log += out[i].len;
	}
	out[n - 1].flags |= FIEMAP_EXTENT_LAST;
	return n;
}

/*
 * fiemap.c's own definition, not a copy of it. This is a *gate* deciding which
 * cases a property applies to rather than an oracle, so the usual "restate it
 * independently so the test cannot agree with the bug" argument is inverted: a
 * drifted copy here makes the property vacuous or spuriously red.
 */
static bool layout_has_phys(const struct fm_rec *r, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++)
		if (r[i].flags & FIEMAP_NO_PHYS)
			return false;
	return true;
}

/*
 * The shape #186 is about: one dedupe stopped on a block boundary, so the same
 * stored extent is described as two records here and one there. Returns the new
 * record count, or `n` unchanged where there is nothing to split.
 */
static unsigned int split_last_record(struct fm_rec *r, unsigned int n)
{
	if (n >= PROP_MAX_RECS || r[n - 1].len <= PROP_BLOCK)
		return n;
	r[n].log = r[n - 1].log + PROP_BLOCK;
	r[n].phys = r[n - 1].phys;
	r[n].len = r[n - 1].len - PROP_BLOCK;
	r[n].flags = r[n - 1].flags;
	r[n - 1].len = PROP_BLOCK;
	r[n - 1].flags &= ~(uint32_t)FIEMAP_EXTENT_LAST;
	return n + 1;
}

/*
 * The convergence property, from the other end than test_dedupe_idempotent.py
 * reaches it: a range always already shares storage with *itself*, so a second
 * dedupe run over an unchanged tree submits nothing. #186 was three separate
 * ways for this to be false, and what made it survive so long is that the
 * summary reports bytes compared rather than bytes freed - the run says it
 * reclaimed gigabytes either way.
 *
 * Excludes only the records whose address means nothing, which the function
 * refuses by design.
 */
MU_TEST(test_prop_maps_share_with_themselves) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		struct fm_rec recs[PROP_MAX_RECS];
		unsigned int n = gen_layout(&p, recs);
		uint64_t end = recs[n - 1].log + recs[n - 1].len;
		uint64_t off = PROP_BLOCK * prop_below(&p, 3);
		uint64_t len = PROP_BLOCK * prop_range(&p, 1, end / PROP_BLOCK + 2);

		if (!layout_has_phys(recs, n))
			continue;
		prop_check(&p, share(recs, n, off, recs, n, off, len));
	}
}

/*
 * A record the kernel cannot pin down poisons the whole comparison, wherever
 * it sits. Deduping past one would be submitted against an address that names
 * nothing - and unlike a miss, which costs one redundant ioctl, believing it
 * skips a dedupe that was real.
 */
MU_TEST(test_prop_maps_never_share_without_a_real_address) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		struct fm_rec recs[PROP_MAX_RECS];
		unsigned int n = gen_layout(&p, recs);
		unsigned int bad = (unsigned int)prop_below(&p, n);
		uint64_t len;

		switch (prop_below(&p, 3)) {
		case 0: recs[bad].flags |= FIEMAP_EXTENT_UNKNOWN; break;
		case 1: recs[bad].flags |= FIEMAP_EXTENT_DELALLOC; break;
		default: recs[bad].flags |= FIEMAP_EXTENT_DATA_INLINE; break;
		}
		/* Long enough to reach the poisoned record, so the walk has to
		 * meet it rather than stopping short of it. */
		len = recs[bad].log + recs[bad].len;
		prop_check(&p, !share(recs, n, 0, recs, n, 0, len));
	}
}

/*
 * The two measures gate different things - one a skip, one a reported number -
 * and the fixed tests already pin them together at a handful of points. This
 * is the same statement over whatever the generator produces: anything
 * fiemap_maps_share() calls fully shared has, by definition, nothing left to
 * free, and a figure claiming otherwise is #187 again.
 *
 * Only one direction is asserted. The converse - nothing left to free implies
 * fully shared - is not a promise either function makes: fiemap_unshared_bytes
 * matches addresses in any order while fiemap_maps_share walks the two maps in
 * step and refuses what it cannot line up, so a destination holding the
 * target's extents in a different order is 0 bytes and not shared.
 */
MU_TEST(test_prop_shared_ranges_have_nothing_left_to_free) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		struct fm_rec tgt[PROP_MAX_RECS], dst[PROP_MAX_RECS];
		unsigned int nt = gen_layout(&p, tgt), nd;
		uint64_t len;

		/* The destination is mostly a variation on the target, because
		 * two independently drawn layouts almost never share anything
		 * and the interesting half of this property would never run. */
		nd = nt;
		memcpy(dst, tgt, nt * sizeof(*tgt));
		switch (prop_below(&p, 4)) {
		case 0:				/* identical */
			break;
		case 1:				/* one address moved */
			dst[prop_below(&p, nd)].phys += PROP_BLOCK;
			break;
		case 2:				/* a split tail */
			nd = split_last_record(dst, nd);
			break;
		default:			/* something else entirely */
			nd = gen_layout(&p, dst);
			break;
		}

		len = PROP_BLOCK * prop_range(&p, 1, 8);
		if (share(tgt, nt, 0, dst, nd, 0, len))
			prop_check(&p, unshared(tgt, nt, dst, nd, 0, len) == 0);
	}
}

/*
 * What a destination can be credited with is bounded by the range submitted,
 * and a second destination on storage already accounted for adds nothing.
 * Both are #191: crediting two destinations in full for one extent claimed
 * twice what releasing it frees.
 *
 * The second statement holds only where every record has a real address, and
 * the generator produces plenty that do not. A DELALLOC or inline record has
 * no stable address to remember, so it is never merged into the set and every
 * pass credits it again - which is the conservative direction (the figure it
 * feeds is a saving, and over-claiming one is #187) but it does mean the
 * unrestricted form of this property is false by design. What is true
 * unconditionally is that a second pass can never credit *more* than the
 * first, since it is measured against a superset of what has been seen.
 */
MU_TEST(test_prop_unshared_bytes_are_bounded_and_credited_once) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		struct fm_rec tgt[PROP_MAX_RECS], dst[PROP_MAX_RECS];
		unsigned int nt = gen_layout(&p, tgt);
		unsigned int nd = gen_layout(&p, dst);
		uint64_t len = PROP_BLOCK * prop_range(&p, 1, 8);
		struct fiemap *t = mkmap(tgt, nt), *d = mkmap(dst, nd);
		struct fiemap_phys_set seen;
		uint64_t first, again;

		fiemap_phys_set_init(&seen, t);
		first = fiemap_unshared_bytes(&seen, d, 0, len);
		again = fiemap_unshared_bytes(&seen, d, 0, len);
		fiemap_phys_set_free(&seen);
		free(t);
		free(d);

		prop_check(&p, first <= len);
		prop_check(&p, again <= first);
		if (layout_has_phys(dst, nd))
			prop_check(&p, again == 0);
	}
}

/*
 * A tiny alphabet on purpose: with three letters and three metacharacters,
 * patterns and paths collide constantly, where a wide alphabet would generate
 * thousands of cases that match nothing and prove nothing.
 */
/* Paths tried against each compiled pattern. Compiling the automaton is nearly
 * the whole cost of a glob case, so this buys path coverage at no extra
 * regexes - and the suite's own speed matters here beyond the usual reason:
 * the mutation tool derives a mutant's hang timeout from how long a green run
 * takes, so a slow suite reclassifies slow mutants as hangs rather than
 * letting a test catch them. */
#define PROP_GLOB_PATHS 8
static void gen_glob_segment(struct prop *p, char *buf, size_t sz)
{
	size_t len = (size_t)prop_range(p, 1, sz - 1);

	for (size_t i = 0; i < len; i++) {
		switch (prop_below(p, 8)) {
		case 0: buf[i] = '*'; break;
		case 1: buf[i] = '?'; break;
		case 2: buf[i] = '.'; break;
		/* '[' and ']' reach append_class(), and an unmatched '[' is the
		 * one way a pattern can fail to compile - which is what makes
		 * gs_of()'s rejection path live. Without them it was dead:
		 * measured 0 rejections in 12,000 compiles, so both the class
		 * parser and the malformed-pattern branch were sampled zero
		 * times while the comment claimed otherwise. */
		case 3: buf[i] = '['; break;
		case 4: buf[i] = ']'; break;
		default: buf[i] = (char)prop_range(p, 'a', 'c'); break;
		}
	}
	buf[len] = '\0';
}

static void gen_glob_path(struct prop *p, char *buf, size_t sz)
{
	unsigned int segs = (unsigned int)prop_range(p, 1, 3);
	size_t o = 0;

	for (unsigned int s = 0; s < segs && o + 6 < sz; s++) {
		size_t len = (size_t)prop_range(p, 1, 3);

		buf[o++] = '/';
		for (size_t i = 0; i < len; i++)
			buf[o++] = prop_chance(p, 6) ? '.'
						     : (char)prop_range(p, 'a', 'c');
	}
	buf[o] = '\0';
}

/* A literal cannot be malformed, so a failure here is a bug in the test, not
 * an input worth skipping - which is why this aborts where gs_of() returns. */
static struct glob_set *gs_literal(const char *path)
{
	struct glob_set *gs = glob_set_new();
	char *err = NULL;

	glob_set_add_literal(gs, path);
	if (glob_set_compile(gs, &err)) {
		fprintf(stderr, "literal \"%s\" rejected: %s\n", path, err);
		abort();
	}
	return gs;
}

/*
 * Build a set from `n` patterns, or NULL if any of them is malformed - which
 * the generator does produce, since '[' comes out of the same draw as the rest.
 * A rejected pattern must not be laundered into "matches nothing": that would
 * make every property below pass vacuously on exactly the inputs where the
 * compiler had something to say.
 */
static struct glob_set *gs_of(const char *const *pats, unsigned int n)
{
	struct glob_set *gs = glob_set_new();
	char *err = NULL;

	for (unsigned int i = 0; i < n; i++) {
		if (glob_set_add(gs, pats[i], &err)) {
			g_free(err);
			glob_set_free(gs);
			return NULL;
		}
	}
	if (glob_set_compile(gs, &err)) {
		g_free(err);
		glob_set_free(gs);
		return NULL;
	}
	return gs;
}

/*
 * The rule the 1.6.0 break was made for (#147): a pattern with no '/' is about
 * the basename and nothing else. Stated without reimplementing the matcher -
 * the path and its own last component have to give the same answer, whatever
 * that answer is - so this cannot pass by agreeing with a second copy of the
 * same bug.
 *
 * `**` is excluded from the pattern, and finding out why is what this property
 * was worth writing for. glob.h states two rules that collide: a pattern with
 * no '/' is a basename rule, and `**` crosses directory boundaries. Here the
 * second wins, so `a**` matches `/a/bbc` - crossing the slash, against a
 * basename that is `bbc` - while `/bbc` alone does not match. git resolves the
 * same collision the other way: in gitignore `**` is only special as a whole
 * path component, and inside one it means `*`. Nobody writes `node**` on
 * purpose and the divergence only ever excludes more than was asked for, so it
 * is left alone here rather than fixed in a test commit; excluded by name so
 * that the property states what is actually promised.
 */
static void squash_doublestars(char *s)
{
	char *w = s;

	for (const char *r = s; *r; r++)
		if (*r != '*' || w == s || w[-1] != '*')
			*w++ = *r;
	*w = '\0';
}

MU_TEST(test_prop_a_bare_pattern_reads_only_the_basename) {
	declare_prop(p, 3000);

	if (prop_skip_jit(__func__))
		return;

	while (prop_next(&p)) {
		char pat[6];
		const char *pats[] = { pat };
		struct glob_set *gs;

		gen_glob_segment(&p, pat, sizeof(pat));
		squash_doublestars(pat);
		gs = gs_of(pats, 1);
		if (!gs)
			continue;

		/* Several paths per compile: building the automaton is nearly
		 * all of what a case costs here - a GRegex compile against a
		 * matcher call - and paths are the axis worth sampling. */
		for (unsigned int k = 0; k < PROP_GLOB_PATHS; k++) {
			char path[24], base[sizeof(path) + 1];
			bool is_dir = prop_bool(&p);
			const char *slash;

			gen_glob_path(&p, path, sizeof(path));
			slash = strrchr(path, '/');
			snprintf(base, sizeof(base), "/%s",
				 slash ? slash + 1 : path);
			prop_check(&p, glob_set_match(gs, path, is_dir, NULL) ==
				       glob_set_match(gs, base, is_dir, NULL));
		}
		glob_set_free(gs);
	}
}

/*
 * --exclude is a union, so adding a pattern can only ever exclude more. The
 * failure this rules out is a shared automaton in which one pattern's
 * compilation changes another's - the patterns are joined into a single
 * regex, and a fragment that leaks a group or an alternation past its own
 * boundary would do exactly that.
 */
MU_TEST(test_prop_adding_a_pattern_never_unexcludes_a_path) {
	declare_prop(p, 3000);

	if (prop_skip_jit(__func__))
		return;

	while (prop_next(&p)) {
		char a[6], b[6];
		const char *one[] = { a };
		const char *both[] = { a, b };
		struct glob_set *gs1, *gs2;

		gen_glob_segment(&p, a, sizeof(a));
		gen_glob_segment(&p, b, sizeof(b));
		gs1 = gs_of(one, 1);
		gs2 = gs_of(both, 2);
		if (!gs1 || !gs2) {
			glob_set_free(gs1);
			glob_set_free(gs2);
			continue;
		}
		for (unsigned int k = 0; k < PROP_GLOB_PATHS; k++) {
			char path[24];
			bool is_dir = prop_bool(&p);

			gen_glob_path(&p, path, sizeof(path));
			if (glob_set_match(gs1, path, is_dir, NULL))
				prop_check(&p, glob_set_match(gs2, path, is_dir, NULL));
		}
		glob_set_free(gs1);
		glob_set_free(gs2);
	}
}

/*
 * A trailing '/' restricts a pattern to directories, and the walk hands
 * `is_dir` straight from the stat. Getting this backwards would exclude every
 * *file* a `cache/` pattern named and none of the directories - and the run
 * would look like it worked, since something was excluded.
 */
MU_TEST(test_prop_a_directory_pattern_never_matches_a_file) {
	declare_prop(p, 3000);

	if (prop_skip_jit(__func__))
		return;

	while (prop_next(&p)) {
		char seg[6], pat[sizeof(seg) + 2];
		const char *pats[] = { pat };
		struct glob_set *gs;

		gen_glob_segment(&p, seg, sizeof(seg));
		snprintf(pat, sizeof(pat), "%s/", seg);
		gs = gs_of(pats, 1);
		if (!gs)
			continue;
		for (unsigned int k = 0; k < PROP_GLOB_PATHS; k++) {
			char path[24];

			gen_glob_path(&p, path, sizeof(path));
			prop_check(&p, !glob_set_match(gs, path, false, NULL));
		}
		glob_set_free(gs);
	}
}

/*
 * A literal is exempt from metacharacter interpretation, which is what keeps a
 * hashfile called `db[1].hash` from being read as a character class - oans adds
 * its own hashfile and the two WAL sidecars this way. So it must match that one
 * path and, being an absolute path rather than a basename rule, nothing else.
 */
MU_TEST(test_prop_a_literal_path_matches_itself_and_nothing_else) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char lit[24], other[24];
		struct glob_set *gs;
		bool is_dir = prop_bool(&p);
		size_t n;

		gen_glob_path(&p, lit, sizeof(lit));
		/* Metacharacters in the *literal*, which is the case it exists
		 * for: as a pattern this would be a class or a wildcard. */
		n = strlen(lit);
		if (n + 3 < sizeof(lit) && prop_bool(&p)) {
			lit[n] = prop_bool(&p) ? '[' : '*';
			lit[n + 1] = 'a';
			lit[n + 2] = '\0';
		}
		gen_glob_path(&p, other, sizeof(other));

		gs = gs_literal(lit);
		prop_check(&p, glob_set_match(gs, lit, is_dir, NULL));
		if (strcmp(lit, other))
			prop_check(&p, !glob_set_match(gs, other, is_dir, NULL));
		glob_set_free(gs);
	}
}

/*
 * The FIDEDUPERANGE probe's answer taxonomy (#224). Worth testing directly and
 * exhaustively: the probe's whole job is turning one ioctl result into a
 * verdict, and what a filesystem returns is exactly what a test host cannot
 * vary. Both wrong answers are bad in different ways -- a wrong NO silently
 * skips a whole tree, a wrong YES brings back per-file dedupe errors -- so
 * anything that is not a clear statement about the filesystem stays UNKNOWN.
 */
/*
 * ---------------------------------------------------------------------------
 * Properties of the hashfile
 *
 * The tables above name a layout and its answer. These name the two
 * relationships that decide whether a snapshot-aware scan is safe at all, and
 * go looking for a layout that breaks them - which is the right shape here,
 * because what makes a layout interesting (a hole, two records that abut, a
 * split at a block boundary) is a fact about extents rather than one anybody
 * sits down and enumerates.
 * ---------------------------------------------------------------------------
 */

/* One donor per case, since a file may hold only one set of extent rows. */
static int64_t prop_donor(struct dbhandle *db, struct prop *p)
{
	char name[64];

	snprintf(name, sizeof(name), "/snap/%u", p->iteration);
	return put_file(db, name, 100000 + p->iteration, 1);
}

/* The fiemap records, as the rows a scan would have stored for them. */
static void rows_of(const struct fm_rec *recs, unsigned int n,
		    struct extent_csum *out)
{
	for (unsigned int i = 0; i < n; i++) {
		out[i].loff = recs[i].log;
		out[i].poff = recs[i].phys;
		out[i].len = recs[i].len;
		memset(out[i].digest, (int)i + 1, DIGEST_LEN);
	}
}

/*
 * The re-check accepts exactly the layout it was given and nothing else.
 *
 * Misses are free and false hits are catastrophic: a wrong match copies one
 * file's digest onto another, and nothing downstream can tell that from a file
 * with no duplicate. So both halves are asserted for every generated layout -
 * that the records it stored match, and that no single-field change to any one
 * record still does.
 *
 * Not a tautology: one side is a `struct fiemap`, the other is the rows
 * dbfile_store_extent_hashes() wrote, and only the loop under test relates
 * them. A mutation inside it makes the two sides disagree rather than agreeing
 * with each other.
 */
MU_TEST(test_prop_a_layout_matches_only_itself) {
	declare_prop(p, 300);
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();

	while (prop_next(&p)) {
		struct fm_rec recs[PROP_MAX_RECS], probe[PROP_MAX_RECS];
		struct extent_csum rows[PROP_MAX_RECS];
		unsigned int n = gen_layout(&p, recs);
		int64_t donor = prop_donor(db, &p);
		unsigned int k, field;

		rows_of(recs, n, rows);
		if (dbfile_store_extent_hashes(db, donor, n, rows))
			abort();

		memcpy(probe, recs, n * sizeof(*probe));
		prop_check(&p, layout_matches(db, donor, probe, n) == 1);

		/* One field of one record, moved by one block. */
		k = (unsigned int)prop_below(&p, n);
		field = (unsigned int)prop_below(&p, 3);
		if (field == 0)
			probe[k].log += PROP_BLOCK;
		else if (field == 1)
			probe[k].phys += PROP_BLOCK;
		else
			probe[k].len += PROP_BLOCK;
		prop_check(&p, layout_matches(db, donor, probe, n) == 0);

		/* A prefix of the donor is still a miss, both directions. */
		memcpy(probe, recs, n * sizeof(*probe));
		if (n > 1)
			prop_check(&p, layout_matches(db, donor, probe, n - 1) == 0);
	}
}

/*
 * The same *storage* described with different record boundaries reads as a
 * miss. Splitting one record in two covers byte for byte what the donor
 * covers, at the same addresses - so a check that reasoned about coverage
 * would say yes, and it must say no (#186 is the same confusion from the other
 * side, where treating two descriptions as different cost convergence).
 *
 * Its own property rather than a branch of the one above, because it is the
 * one case where "obviously the same bytes" and "the same records" part
 * company, and a generator has to be told to produce it.
 */
MU_TEST(test_prop_a_split_record_is_not_the_layout_it_covers) {
	declare_prop(p, 300);
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();

	while (prop_next(&p)) {
		struct fm_rec recs[PROP_MAX_RECS], probe[PROP_MAX_RECS + 1];
		struct extent_csum rows[PROP_MAX_RECS];
		unsigned int n = gen_layout(&p, recs);
		int64_t donor;
		unsigned int k = (unsigned int)prop_below(&p, n);
		unsigned int i, j;
		uint64_t half;

		if (recs[k].len < 2 * PROP_BLOCK)
			continue;		/* nothing to split */
		half = recs[k].len / 2;

		donor = prop_donor(db, &p);
		rows_of(recs, n, rows);
		if (dbfile_store_extent_hashes(db, donor, n, rows))
			abort();

		for (i = 0, j = 0; i < n; i++) {
			probe[j++] = recs[i];
			if (i != k)
				continue;
			probe[j - 1].len = half;
			probe[j] = recs[i];
			probe[j].log += half;
			probe[j].len -= half;
			/* The address is left alone: fe_physical addresses a
			 * whole extent, so an offset into it means nothing on
			 * a compressed one. Same trap as fiemap_maps_share. */
			j++;
		}
		prop_check(&p, layout_matches(db, donor, probe, j) == 0);
	}
}

/*
 * Everything handed to dbfile_store_extent_hashes() comes back at the offsets
 * it was given, and nothing else does.
 *
 * The second clause is what makes the zero-length skip a property rather than
 * a case: an extent naming no bytes must not be stored at any count, at any
 * position in the array, including as the only element. The table above can
 * only ask that at the two shapes it writes down.
 *
 * Read back with SQL rather than through a loader, so a bind index shifted
 * consistently in both directions cannot round-trip.
 */
MU_TEST(test_prop_stored_extents_come_back_where_they_were_put) {
	declare_prop(p, 300);
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();

	while (prop_next(&p)) {
		struct extent_csum ext[6];
		unsigned int n = (unsigned int)prop_range(&p, 0, ARRAY_SIZE(ext));
		uint64_t loff = 0, expect = 0;
		int64_t id = prop_donor(db, &p);
		char sql[256];

		for (unsigned int i = 0; i < n; i++) {
			ext[i].loff = loff;
			ext[i].poff = PROP_BLOCK * prop_range(&p, 100, 108);
			/* Empty about one record in five, so the skip is
			 * reached first, last and alone across the run. */
			ext[i].len = prop_chance(&p, 5)
				   ? 0 : PROP_BLOCK * prop_range(&p, 1, 3);
			memset(ext[i].digest, (int)prop_u64(&p), DIGEST_LEN);
			loff += PROP_BLOCK * prop_range(&p, 1, 4);
			if (ext[i].len)
				expect++;
		}

		if (dbfile_store_extent_hashes(db, id, n, ext))
			abort();

		for (unsigned int i = 0; i < n; i++) {
			if (!ext[i].len)
				continue;
			snprintf(sql, sizeof(sql),
				 "select count(*) from extents where "
				 "fileid = %lld and loff = %llu and "
				 "poff = %llu and len = %llu",
				 (long long)id, (unsigned long long)ext[i].loff,
				 (unsigned long long)ext[i].poff,
				 (unsigned long long)ext[i].len);
			prop_check(&p, dbfile_query_u64(db->db, sql) == 1);
		}

		snprintf(sql, sizeof(sql),
			 "select count(*) from extents where fileid = %lld",
			 (long long)id);
		prop_check(&p, dbfile_query_u64(db->db, sql) == expect);
	}
}

/*
 * ---------------------------------------------------------------------------
 * The in-memory dedupe model: filerecs, the hash tree, the results tree
 *
 * All three are pure memory - no filesystem, no btrfs, no scan - and all three
 * were at a 0% mutation kill rate before this: 502 mutants, nothing caught.
 * They are also the only callers of the vendored kernel rbtree, so generated
 * insert and *erase* orders here are what exercise that copy's rebalancing;
 * inserting and removing in one ascending sweep would only ever walk a spine.
 * ---------------------------------------------------------------------------
 */

#define PROP_FILES	4
#define PROP_DIGESTS	5
#define PROP_BLOCKS	24

/*
 * Offsets and extent lengths use their own constant rather than the global
 * `blocksize`, which an earlier test sets to 100 and never restores. Nothing
 * here depends on the value, only on it being fixed.
 */
#define PROP_LEN	4096

/* Takes the fileid it will actually have: the tests look filerecs up by id,
 * so a helper that invented its own numbering had to be bypassed by the one
 * property that chooses ids. */
static struct filerec *mkfilerec(int64_t fileid)
{
	char name[64];
	struct filerec *f;

	snprintf(name, sizeof(name), "/tree/f%lld", (long long)fileid);
	f = filerec_new(name, fileid, PROP_LEN * (uint64_t)fileid);
	if (!f)
		abort();
	return f;
}

static void prop_shuffle_u64(struct prop *p, uint64_t *a, unsigned int n)
{
	for (unsigned int i = n; i > 1; i--) {
		unsigned int j = (unsigned int)prop_below(p, i);
		uint64_t t = a[i - 1];

		a[i - 1] = a[j];
		a[j] = t;
	}
}

MU_TEST(test_filerec_the_registry_finds_what_was_put_in_it) {
	struct filerec *a, *b;

	/* One global registry serves every test, so each starts it over. */
	free_all_filerecs();
	a = mkfilerec(1);
	b = mkfilerec(2);

	mu_check(filerec_find(1) == a);
	mu_check(filerec_find(2) == b);
	mu_check(filerec_find(99) == NULL);	/* never inserted */

	mu_check(!strcmp(a->filename, "/tree/f1"));
	mu_check(a->size == PROP_LEN);
	mu_check(a->fd == -1);			/* not opened yet */

	free_all_filerecs();
	mu_check(filerec_find(1) == NULL);	/* and teardown really clears */
}

/*
 * The reference count balances, and the free that ends it takes the filerec
 * out of the lookup tree - so a later filerec_find() cannot hand back a
 * pointer to freed memory.
 *
 * Deliberately *not* claiming this pins the cross-batch lifetime rule from
 * CLAUDE.md. That rule is about run_dedupe.c holding a ref per batch, and it
 * is not even true of this module in isolation: free_all_filerecs() frees
 * every filerec whatever its refs, which is what the teardown on the last
 * line of this test does. What is testable here is the smaller claim above.
 */
MU_TEST(test_filerec_is_unfindable_once_its_last_reference_goes) {
	struct filerec *f;

	free_all_filerecs();
	f = mkfilerec(1);
	mu_check(f->refs == 0);		/* created unreferenced */

	filerec_get(f);
	filerec_get(f);
	mu_check(f->refs == 2);

	filerec_put(f);
	mu_check(f->refs == 1);
	mu_check(filerec_find(1) == f);	/* one holder left: still live */

	filerec_put(f);
	mu_check(filerec_find(1) == NULL);

	free_all_filerecs();
}

/*
 * Every filerec is findable by its own id and by no other, for any insertion
 * order. The registry is an rbtree keyed on fileid, so this is what makes
 * cmp_filerecs' answer observable: a comparator that inverts, or that reads
 * the wrong field, still builds *a* tree - just one that cannot find things
 * again.
 */
MU_TEST(test_prop_every_filerec_is_findable_by_its_own_id) {
	declare_prop(p, 200);

	while (prop_next(&p)) {
		uint64_t ids[PROP_BLOCKS];
		struct filerec *made[PROP_BLOCKS];
		unsigned int n = (unsigned int)prop_range(&p, 1, ARRAY_SIZE(ids));
		uint64_t id = 0;

		free_all_filerecs();

		/* Ascending with gaps makes them distinct by construction; the
		 * shuffle is what makes the insertion order arbitrary. The gaps
		 * double as ids to look up and miss on. */
		for (unsigned int i = 0; i < n; i++) {
			id += prop_range(&p, 1, 8);
			ids[i] = id;
		}
		prop_shuffle_u64(&p, ids, n);

		for (unsigned int i = 0; i < n; i++)
			made[i] = mkfilerec((int64_t)ids[i]);

		for (unsigned int i = 0; i < n; i++) {
			prop_check(&p, filerec_find((int64_t)ids[i]) == made[i]);
			prop_check(&p, made[i]->fileid == (int64_t)ids[i]);
		}

		/* Past every id inserted: not answered with a neighbour. */
		for (uint64_t probe = id + 1; probe <= id + 4; probe++)
			prop_check(&p, filerec_find((int64_t)probe) == NULL);
	}
	free_all_filerecs();
}

/*
 * Everything the hash tree counts stays consistent with what is in it, for any
 * mix of files, digests and offsets.
 *
 * Three counters must agree with three different things: tree->num_blocks with
 * the blocks inserted, tree->num_hashes with the *distinct* digests, and each
 * dl_num_elem with that digest's share. Nothing downstream validates them;
 * find_dupes reads them to decide what to compare, so a count that drifts
 * makes oans quietly compare the wrong set.
 *
 * The shadow model (per_digest, next_loff) is kept independently of the tree
 * rather than read back out of it, which is what stops this being a
 * restatement of the code under test.
 */
MU_TEST(test_prop_the_hash_tree_counts_what_it_holds) {
	declare_prop(p, 120);
	struct filerec *files[PROP_FILES];
	unsigned char digests[PROP_DIGESTS][DIGEST_LEN];

	/* Loop-invariant: the tree is torn down each iteration, and that walks
	 * every block out of the filerecs' own trees, so these come back
	 * clean. */
	free_all_filerecs();
	for (unsigned int i = 0; i < PROP_FILES; i++)
		files[i] = mkfilerec((int64_t)i + 1);
	for (unsigned int i = 0; i < PROP_DIGESTS; i++)
		digest_of(digests[i], i);

	while (prop_next(&p)) {
		struct hash_tree tree;
		unsigned int per_digest[PROP_DIGESTS] = {0};
		unsigned int nb = (unsigned int)prop_range(&p, 1, PROP_BLOCKS);
		unsigned int distinct = 0;
		uint64_t next_loff[PROP_FILES] = {0};

		init_hash_tree(&tree);

		for (unsigned int i = 0; i < nb; i++) {
			unsigned int f = (unsigned int)prop_below(&p, PROP_FILES);
			unsigned int d = (unsigned int)prop_below(&p, PROP_DIGESTS);

			/* Distinct offsets per file: the filerec block tree is
			 * keyed on the offset, and two blocks of one file at one
			 * offset is not a shape the scan produces. */
			if (insert_hashed_block(&tree, digests[d], files[f],
						next_loff[f]))
				abort();
			next_loff[f] += PROP_LEN;
			if (per_digest[d]++ == 0)
				distinct++;
		}

		prop_check(&p, tree.num_blocks == nb);
		prop_check(&p, tree.num_hashes == distinct);

		for (unsigned int d = 0; d < PROP_DIGESTS; d++) {
			struct dupe_blocks_list *dl =
				find_block_list(&tree, digests[d]);

			if (!per_digest[d]) {
				/* Never inserted: not answered with a
				 * neighbouring digest's list. */
				prop_check(&p, dl == NULL);
				continue;
			}
			prop_check(&p, dl != NULL);
			prop_check(&p, dl->dl_num_elem == per_digest[d]);
			prop_check(&p, !memcmp(dl->dl_hash, digests[d], DIGEST_LEN));
		}

		for (unsigned int f = 0; f < PROP_FILES; f++) {
			for (uint64_t off = 0; off < next_loff[f]; off += PROP_LEN) {
				struct file_block *b =
					find_filerec_block(files[f], off);

				prop_check(&p, b != NULL);
				prop_check(&p, b->b_loff == off);
				prop_check(&p, b->b_file == files[f]);
			}
			prop_check(&p, find_filerec_block(files[f],
							  next_loff[f]) == NULL);
		}

		free_hash_tree(&tree);
	}
	free_all_filerecs();
}

/*
 * Taking blocks back out, in an order unrelated to the one they went in.
 *
 * Two things here that a simpler shape cannot see. remove_hashed_block answers
 * 1 exactly when it removed the *last* block carrying that digest and freed
 * the list - find_dupes uses that to know the group is gone, so an answer
 * right about the tree and wrong about the verdict leaves a caller holding a
 * freed list. And a file_hash_head is freed when *that file's* sublist drains,
 * which with a single file would always coincide with the list itself being
 * freed; several files make the two instants different, so dropping the head
 * free leaks it somewhere a counter can still notice.
 *
 * The removal order is generated, which is also the only thing in these tests
 * that makes the vendored rbtree rebalance on erase rather than unlink a
 * spine.
 */
MU_TEST(test_prop_removing_every_block_empties_the_hash_tree) {
	declare_prop(p, 120);
	struct filerec *files[PROP_FILES];
	unsigned char digests[PROP_DIGESTS][DIGEST_LEN];

	free_all_filerecs();
	for (unsigned int i = 0; i < PROP_FILES; i++)
		files[i] = mkfilerec((int64_t)i + 1);
	for (unsigned int i = 0; i < PROP_DIGESTS; i++)
		digest_of(digests[i], i);

	while (prop_next(&p)) {
		struct hash_tree tree;
		unsigned int per_digest[PROP_DIGESTS] = {0};
		unsigned int per_pair[PROP_DIGESTS][PROP_FILES] = {{0}};
		unsigned int wd[PROP_BLOCKS], wf[PROP_BLOCKS];
		uint64_t order[PROP_BLOCKS];
		unsigned int nb = (unsigned int)prop_range(&p, 1, PROP_BLOCKS);
		unsigned int distinct = 0;
		uint64_t next_loff[PROP_FILES] = {0};
		uint64_t loffs[PROP_BLOCKS];

		init_hash_tree(&tree);

		for (unsigned int i = 0; i < nb; i++) {
			unsigned int f = (unsigned int)prop_below(&p, PROP_FILES);
			unsigned int d = (unsigned int)prop_below(&p, PROP_DIGESTS);

			wd[i] = d;
			wf[i] = f;
			loffs[i] = next_loff[f];
			order[i] = i;
			if (insert_hashed_block(&tree, digests[d], files[f],
						next_loff[f]))
				abort();
			next_loff[f] += PROP_LEN;
			per_pair[d][f]++;
			if (per_digest[d]++ == 0)
				distinct++;
		}

		prop_shuffle_u64(&p, order, nb);

		for (unsigned int k = 0; k < nb; k++) {
			unsigned int i = (unsigned int)order[k];
			unsigned int d = wd[i], f = wf[i];
			struct dupe_blocks_list *dl =
				find_block_list(&tree, digests[d]);
			struct file_block *b =
				find_filerec_block(files[f], loffs[i]);
			bool last_of_digest = per_digest[d] == 1;
			bool last_of_pair = per_pair[d][f] == 1;
			int ret;

			prop_check(&p, dl != NULL);
			prop_check(&p, b != NULL);

			per_digest[d]--;
			per_pair[d][f]--;
			ret = remove_hashed_block(&tree, b);
			prop_check(&p, ret == (last_of_digest ? 1 : 0));

			/* That file's head is gone once its share drains, while
			 * the list itself lives on for the other files. */
			if (!last_of_digest)
				prop_check(&p,
					   (find_file_hash_head(dl, files[f])
					    == NULL) == last_of_pair);

			if (last_of_digest)
				distinct--;
			prop_check(&p, tree.num_hashes == distinct);
			prop_check(&p, tree.num_blocks == nb - k - 1);
			prop_check(&p, find_filerec_block(files[f],
							  loffs[i]) == NULL);
		}

		prop_check(&p, tree.num_blocks == 0 && tree.num_hashes == 0);
		prop_check(&p, RB_EMPTY_ROOT(&tree.root));
		free_hash_tree(&tree);
	}
	free_all_filerecs();
}

/*
 * find_dupes walks each file's blocks under a hash expecting increasing
 * offsets, and says so where sort_file_hash_heads is declared. The blocks
 * arrive that way only because the scan happens to produce them so, which is
 * why the sort exists - and why a fixture that inserts in order cannot tell a
 * working sort from no sort at all. Hence a shuffled insertion order.
 *
 * Two digests, not one: the sort is two nested walks, and with a single block
 * list the outer one runs exactly once for the life of the property, so a
 * mutant that stops after the first list would be invisible.
 */
MU_TEST(test_prop_sorting_puts_every_hash_head_in_offset_order) {
	declare_prop(p, 120);
	struct filerec *files[PROP_FILES];
	unsigned char digests[2][DIGEST_LEN];

	free_all_filerecs();
	for (unsigned int i = 0; i < PROP_FILES; i++)
		files[i] = mkfilerec((int64_t)i + 1);
	digest_of(digests[0], 0);
	digest_of(digests[1], 1);

	while (prop_next(&p)) {
		struct hash_tree tree;
		uint64_t offs[PROP_BLOCKS];
		unsigned int nb = (unsigned int)prop_range(&p, 2, PROP_BLOCKS);
		unsigned int heads = 0;
		struct rb_node *dn;

		init_hash_tree(&tree);
		for (unsigned int i = 0; i < nb; i++)
			offs[i] = PROP_LEN * i;
		prop_shuffle_u64(&p, offs, nb);

		for (unsigned int i = 0; i < nb; i++) {
			unsigned int f = (unsigned int)prop_below(&p, PROP_FILES);
			unsigned int d = (unsigned int)prop_below(&p, 2);

			if (insert_hashed_block(&tree, digests[d], files[f],
						offs[i]))
				abort();
		}

		sort_file_hash_heads(&tree);

		for (dn = rb_first(&tree.root); dn; dn = rb_next(dn)) {
			struct dupe_blocks_list *dl =
				rb_entry(dn, struct dupe_blocks_list, dl_node);
			struct rb_node *n;

			for (n = rb_first(&dl->dl_files_root); n; n = rb_next(n)) {
				struct file_hash_head *h =
					rb_entry(n, struct file_hash_head, h_node);
				struct file_block *b;
				uint64_t prev = 0;
				bool first = true;

				heads++;
				list_for_each_entry(b, &h->h_blocks, b_head_list) {
					prop_check(&p, b->b_file == h->h_file);
					prop_check(&p, first || b->b_loff > prev);
					prev = b->b_loff;
					first = false;
				}
			}
		}
		/* Every list reached, not just the first. */
		prop_check(&p, heads >= tree.num_hashes);

		free_hash_tree(&tree);
	}
	free_all_filerecs();
}

/*
 * A results tree's groups and their members stay consistent with what was
 * inserted, across several digests *and* several lengths.
 *
 * A group is keyed on (digest, length) together, so drawing both is what makes
 * the keying observable at all - with one digest at one length every case
 * builds a one-node tree, num_dupes is never in question, and the comparator's
 * second level never runs.
 *
 * dext_work() is the single definition of a group's work and four consumers
 * depend on them agreeing: the largest-first sort key, the per-thread status
 * total, the byte-progress settlement target and the upfront SQL total. It is
 * restated here as arithmetic rather than called, so a change to the formula
 * makes the two sides disagree instead of moving together.
 */
MU_TEST(test_prop_a_dup_group_counts_its_own_members) {
	declare_prop(p, 150);
#define PROP_LENS	3
	struct filerec *files[PROP_FILES];
	unsigned char digests[PROP_DIGESTS][DIGEST_LEN];
	static const uint64_t lens[PROP_LENS] = {
		PROP_LEN, PROP_LEN * 2, PROP_LEN * 3
	};

	free_all_filerecs();
	for (unsigned int i = 0; i < PROP_FILES; i++)
		files[i] = mkfilerec((int64_t)i + 1);
	for (unsigned int i = 0; i < PROP_DIGESTS; i++)
		digest_of(digests[i], i);

	while (prop_next(&p)) {
		struct results_tree res;
		unsigned int n = (unsigned int)prop_range(&p, 1, 16);
		unsigned int unique = 0, groups = 0;
		/* Shadow model: which (digest, len) groups exist, and which
		 * (digest, len, file, slot) extents are in them. */
		bool has_group[PROP_DIGESTS][PROP_LENS] = {{false}};
		bool seen[PROP_DIGESTS][PROP_LENS][PROP_FILES][4];
		unsigned int members[PROP_DIGESTS][PROP_LENS] = {{0}};
		struct rb_node *node;

		init_results_tree(&res);
		memset(seen, 0, sizeof(seen));

		for (unsigned int i = 0; i < n; i++) {
			unsigned int d = (unsigned int)prop_below(&p, PROP_DIGESTS);
			unsigned int l = (unsigned int)prop_below(&p, PROP_LENS);
			unsigned int f = (unsigned int)prop_below(&p, PROP_FILES);
			unsigned int slot = (unsigned int)prop_below(&p, 4);

			/*
			 * Repeats on purpose: the same (file, offset) twice is
			 * one extent, not two, and the second insert must free
			 * its own allocation rather than counting it.
			 */
			if (insert_one_result(&res, digests[d], files[f],
					      slot * PROP_LEN * 4, lens[l],
					      0, false))
				abort();
			if (!has_group[d][l]) {
				has_group[d][l] = true;
				groups++;
			}
			if (!seen[d][l][f][slot]) {
				seen[d][l][f][slot] = true;
				members[d][l]++;
				unique++;
			}
		}

		prop_check(&p, res.num_extents == unique);
		prop_check(&p, res.num_dupes == groups);

		/* Each group is findable under its own key, and holds exactly
		 * the members the model says. */
		for (unsigned int d = 0; d < PROP_DIGESTS; d++) {
			for (unsigned int l = 0; l < PROP_LENS; l++) {
				struct dupe_extents *g =
					find_dupe_extents(&res, digests[d], lens[l]);

				if (!has_group[d][l]) {
					prop_check(&p, g == NULL);
					continue;
				}
				prop_check(&p, g != NULL);
				prop_check(&p, g->de_num_dupes == members[d][l]);
				prop_check(&p, g->de_len == lens[l]);
			}
		}

		/* The list, the rbtree and the counter are three views of one
		 * membership and must never disagree. */
		for (node = rb_first(&res.root); node; node = rb_next(node)) {
			struct dupe_extents *d =
				rb_entry(node, struct dupe_extents, de_node);
			struct extent *e;
			unsigned int listed = 0, in_tree = 0;
			struct rb_node *m;

			list_for_each_entry(e, &d->de_extents, e_list) {
				prop_check(&p, e->e_parent == d);
				listed++;
			}
			for (m = rb_first(&d->de_extents_root); m; m = rb_next(m))
				in_tree++;

			prop_check(&p, listed == d->de_num_dupes);
			prop_check(&p, in_tree == d->de_num_dupes);
			prop_check(&p, dext_work(d) ==
				   d->de_len * (d->de_num_dupes - 1));
		}

		free_results_tree(&res);
	}
	free_all_filerecs();
}

/*
 * remove_extent's cascade, which is the part a fixture gets wrong.
 *
 * A group of one is meaningless - there is nothing left to dedupe against - so
 * dropping to one member removes that member too and frees the group. Removing
 * from a pair therefore takes *both* extents and answers 0, where a straight
 * decrement would say 1. A test written against a three-member group alone
 * never reaches the cascade.
 */
MU_TEST(test_removing_an_extent_collapses_a_group_of_one) {
	struct results_tree res;
	unsigned char digest[DIGEST_LEN];
	struct dupe_extents *d;
	struct extent *e;

	free_all_filerecs();
	init_results_tree(&res);
	digest_of(digest, 1);

	for (unsigned int i = 0; i < 3; i++)
		mu_check(insert_one_result(&res, digest, mkfilerec(i + 1), 0,
					   PROP_LEN, 0, false) == 0);
	mu_check(res.num_extents == 3 && res.num_dupes == 1);

	/* Three members: one comes out, two remain, and it says so. */
	d = find_dupe_extents(&res, digest, PROP_LEN);
	mu_check(d && d->de_num_dupes == 3);
	e = list_first_entry(&d->de_extents, struct extent, e_list);
	mu_check(remove_extent(&res, e) == 2);
	mu_check(res.num_extents == 2);
	mu_check(res.num_dupes == 1);		/* the group survives */

	/* Two members: removing one leaves a group of one, which goes too. */
	d = find_dupe_extents(&res, digest, PROP_LEN);
	mu_check(d != NULL);
	e = list_first_entry(&d->de_extents, struct extent, e_list);
	mu_check(remove_extent(&res, e) == 0);
	mu_check(res.num_extents == 0);
	mu_check(res.num_dupes == 0);
	mu_check(RB_EMPTY_ROOT(&res.root));
	mu_check(find_dupe_extents(&res, digest, PROP_LEN) == NULL);

	free_results_tree(&res);
	free_all_filerecs();
}

/*
 * Groups are keyed on (digest, length) together, not on digest alone: two runs
 * of identical bytes at different lengths are not duplicates of each other,
 * and insert_one_result abort_on()s a length disagreeing with the group it
 * landed in.
 */
MU_TEST(test_a_group_is_keyed_on_digest_and_length_together) {
	struct results_tree res;
	unsigned char digest[DIGEST_LEN], other[DIGEST_LEN];
	struct filerec *a, *b;

	free_all_filerecs();
	init_results_tree(&res);
	a = mkfilerec(1);
	b = mkfilerec(2);
	digest_of(digest, 1);
	digest_of(other, 2);

	mu_check(insert_one_result(&res, digest, a, 0, PROP_LEN, 0, false) == 0);
	mu_check(insert_one_result(&res, digest, b, 0, PROP_LEN, 0, false) == 0);
	mu_check(res.num_dupes == 1);

	/* Same digest, different length: a second group, not an abort. */
	mu_check(insert_one_result(&res, digest, a, PROP_LEN * 2, PROP_LEN * 2,
				   0, false) == 0);
	mu_check(res.num_dupes == 2);

	/* Different digest, same length: a third. */
	mu_check(insert_one_result(&res, other, a, PROP_LEN * 8, PROP_LEN, 0,
				   false) == 0);
	mu_check(res.num_dupes == 3);
	mu_check(res.num_extents == 4);

	/* Each is findable under its own key and not under the other's. */
	mu_check(find_dupe_extents(&res, digest, PROP_LEN) != NULL);
	mu_check(find_dupe_extents(&res, digest, PROP_LEN * 2) != NULL);
	mu_check(find_dupe_extents(&res, other, PROP_LEN) != NULL);
	mu_check(find_dupe_extents(&res, other, PROP_LEN * 2) == NULL);

	free_results_tree(&res);
	free_all_filerecs();
}

/*
 * The anchor flag latches: once a member claims it, later members that do not
 * cannot clear it. That is what pins a group spanning several dedupe passes to
 * one target instead of re-picking a least-fragmented one per pass.
 *
 * The order matters, and it is the whole test. Asserting it right after the
 * anchored insert would pass just as well against `de_anchored = is_anchor`,
 * because nothing follows to overwrite it - so "sticks" is only a claim if a
 * non-anchored insert comes afterwards.
 */
MU_TEST(test_the_anchor_flag_latches_once_claimed) {
	struct results_tree res;
	unsigned char digest[DIGEST_LEN];
	struct dupe_extents *d;

	free_all_filerecs();
	init_results_tree(&res);
	digest_of(digest, 1);

	mu_check(insert_one_result(&res, digest, mkfilerec(1), 0, PROP_LEN, 0,
				   false) == 0);
	d = find_dupe_extents(&res, digest, PROP_LEN);
	mu_check(d && !d->de_anchored);

	mu_check(insert_one_result(&res, digest, mkfilerec(2), 0, PROP_LEN, 0,
				   true) == 0);
	mu_check(d->de_anchored);

	/* The one that matters: a later plain member must not clear it. */
	mu_check(insert_one_result(&res, digest, mkfilerec(3), 0, PROP_LEN, 0,
				   false) == 0);
	mu_check(d->de_anchored);

	free_results_tree(&res);
	free_all_filerecs();
}

/*
 * insert_result() is the pair-wise entry point find_dupes actually calls;
 * insert_one_result() above is the single-extent variant. Testing the helper
 * and not this one left 36 of results-tree.c's mutants untouched.
 *
 * Two things here that only this signature has. The group's length comes from
 * `endoff[0] - startoff[0] + 1` - **inclusive**, and taken from the *first*
 * pair alone - so an off-by-one silently files the pair under a neighbouring
 * key, where it can never meet the extents it duplicates. And both extents go
 * into one group, so a pair that is really the same extent twice must count
 * once.
 */
MU_TEST(test_insert_result_files_a_pair_under_one_length) {
	struct results_tree res;
	unsigned char digest[DIGEST_LEN];
	struct filerec *recs[2];
	uint64_t startoff[2], endoff[2];
	struct dupe_extents *d;

	free_all_filerecs();
	init_results_tree(&res);
	digest_of(digest, 1);
	recs[0] = mkfilerec(1);
	recs[1] = mkfilerec(2);

	/* An inclusive range of exactly PROP_LEN bytes. */
	startoff[0] = 0;
	endoff[0] = PROP_LEN - 1;
	startoff[1] = PROP_LEN * 4;
	endoff[1] = PROP_LEN * 5 - 1;
	mu_check(insert_result(&res, digest, recs, startoff, endoff) == 0);

	mu_check(res.num_dupes == 1);
	mu_check(res.num_extents == 2);
	d = find_dupe_extents(&res, digest, PROP_LEN);
	mu_check(d != NULL);			/* not PROP_LEN - 1, nor + 1 */
	mu_check(d && d->de_num_dupes == 2);
	mu_check(find_dupe_extents(&res, digest, PROP_LEN - 1) == NULL);
	mu_check(find_dupe_extents(&res, digest, PROP_LEN + 1) == NULL);

	/* The second file's offset is its own, not a copy of the first's. */
	{
		struct extent *e;
		bool at_0 = false, at_4 = false;

		list_for_each_entry(e, &d->de_extents, e_list) {
			if (e->e_file == recs[0] && e->e_loff == 0)
				at_0 = true;
			if (e->e_file == recs[1] && e->e_loff == PROP_LEN * 4)
				at_4 = true;
		}
		mu_check(at_0 && at_4);
	}

	/* The same extent named twice is one member, not two. */
	recs[1] = recs[0];
	startoff[1] = startoff[0];
	endoff[1] = endoff[0];
	mu_check(insert_result(&res, digest, recs, startoff, endoff) == 0);
	mu_check(res.num_extents == 2);		/* unchanged */
	mu_check(d->de_num_dupes == 2);

	free_results_tree(&res);
	free_all_filerecs();
}

/* A real file to open. Any filesystem will do - this is fd bookkeeping, not
 * dedupe - so it does not need the reflink scratch dir. */
static struct filerec *mkrealfile(char *path, size_t sz)
{
	int fd;

	snprintf(path, sz, "/tmp/oans-filerec-XXXXXX");
	fd = mkstemp(path);
	if (fd < 0 || write(fd, "hello", 5) != 5)
		abort();
	close(fd);
	return filerec_new(path, 1, 5);
}

/*
 * The file descriptor is reference counted, and that is the whole point: the
 * dedupe phase opens the same file from several groups at once, so a nested
 * open must hand back the descriptor already open rather than a second one.
 * Opening twice and closing twice looks correct either way from the outside -
 * what separates a real refcount from open-and-close-every-time is that the
 * descriptor stays *usable* across the inner close, which is why this reads a
 * byte through it rather than only inspecting the counter.
 */
MU_TEST(test_filerec_holds_one_descriptor_however_often_it_is_opened) {
	char path[64];
	struct filerec *f;
	char buf[1];
	int first_fd;

	free_all_filerecs();
	f = mkrealfile(path, sizeof(path));
	mu_check(f->fd == -1 && f->fd_refs == 0);

	mu_check(filerec_open(f, false) == 0);
	mu_check(f->fd != -1 && f->fd_refs == 1);
	first_fd = f->fd;

	/* Nested open: the same descriptor, not a second one. */
	mu_check(filerec_open(f, false) == 0);
	mu_check(f->fd == first_fd);
	mu_check(f->fd_refs == 2);

	filerec_close(f);
	mu_check(f->fd_refs == 1);
	mu_check(f->fd == first_fd);
	/* Still open, which a close-every-time implementation would fail. */
	mu_check(pread(f->fd, buf, 1, 0) == 1 && buf[0] == 'h');

	filerec_close(f);
	mu_check(f->fd_refs == 0 && f->fd == -1);

	/* A file that is not there reports the errno and leaves the filerec
	 * closed rather than counting a descriptor it never got. */
	unlink(path);
	mu_check(filerec_open(f, true) == ENOENT);
	mu_check(f->fd == -1 && f->fd_refs == 0);

	free_all_filerecs();
}

/*
 * filerec_open_once() is what the dedupe phase uses to open every member of a
 * group without opening any of them twice. It remembers what it has opened in
 * a token tree keyed on the filerec, so asking again is a no-op - and the test
 * of that is the *refcount*, since a second real open would leave a
 * descriptor no close in the batch will ever release.
 */
MU_TEST(test_filerec_open_once_opens_each_file_exactly_once) {
	char path[64];
	struct filerec *f;
	OPEN_ONCE(open_files);

	free_all_filerecs();
	f = mkrealfile(path, sizeof(path));

	mu_check(filerec_open_once(f, &open_files) == 0);
	mu_check(f->fd_refs == 1);

	/* Asked twice, opened once. */
	mu_check(filerec_open_once(f, &open_files) == 0);
	mu_check(f->fd_refs == 1);
	mu_check(filerec_open_once(f, &open_files) == 0);
	mu_check(f->fd_refs == 1);

	filerec_close_open_list(&open_files);
	mu_check(f->fd_refs == 0 && f->fd == -1);
	mu_check(RB_EMPTY_ROOT(&open_files.root));

	/* A missing file is reported, and leaves no token behind claiming it
	 * was opened - otherwise a later pass would skip opening it. */
	unlink(path);
	mu_check(filerec_open_once(f, &open_files) == ENOENT);
	mu_check(RB_EMPTY_ROOT(&open_files.root));
	mu_check(f->fd_refs == 0);

	free_all_filerecs();
}

/*
 * The token tree is keyed on the filerec *pointer*, which is what makes the
 * lookup above cheap. Worth its own case because pointer ordering is the one
 * comparator here whose operands a test cannot choose: the property below
 * inserts whatever addresses the allocator hands out, so it fails only if the
 * comparator disagrees with itself.
 */
MU_TEST(test_prop_a_filerec_token_is_found_by_its_filerec) {
	declare_prop(p, 120);
	struct filerec *files[PROP_FILES];

	free_all_filerecs();
	for (unsigned int i = 0; i < PROP_FILES; i++)
		files[i] = mkfilerec((int64_t)i + 1);

	while (prop_next(&p)) {
		struct rb_root root = RB_ROOT;
		bool inserted[PROP_FILES] = {false};
		uint64_t order[PROP_FILES];
		unsigned int n = (unsigned int)prop_range(&p, 1, PROP_FILES);

		for (unsigned int i = 0; i < PROP_FILES; i++)
			order[i] = i;
		prop_shuffle_u64(&p, order, PROP_FILES);

		for (unsigned int i = 0; i < n; i++) {
			struct filerec *f = files[order[i]];
			struct filerec_token *t = filerec_token_new(f);

			if (!t)
				abort();
			insert_filerec_token_rb(&root, t);
			inserted[order[i]] = true;
		}

		for (unsigned int i = 0; i < PROP_FILES; i++) {
			struct filerec_token *t =
				find_filerec_token_rb(&root, files[i]);

			if (!inserted[i]) {
				prop_check(&p, t == NULL);
				continue;
			}
			prop_check(&p, t != NULL);
			prop_check(&p, t->t_file == files[i]);
		}

		while (!RB_EMPTY_ROOT(&root)) {
			struct filerec_token *t =
				rb_entry(rb_first(&root), struct filerec_token,
					 t_node);

			rb_erase(&t->t_node, &root);
			filerec_token_free(t);
		}
	}
	free_all_filerecs();
}

/*
 * ---------------------------------------------------------------------------
 * The dedupe-phase loaders
 *
 * Where the hashfile meets the in-memory model: five functions turning rows
 * into filerecs, hash trees and results trees. They were 155 mutants with a 0%
 * kill rate, and they need both fixtures at once - memdb() from the hashfile
 * tests, mkfilerec()/free_all_filerecs() from the model ones - which is why
 * they waited until both existed.
 *
 * What they get wrong fails silently in the usual way: a target elected
 * differently in two overlapping windows deduplicates into a file the other
 * window is still relocating, and the summary looks exactly the same.
 *
 * Note when reading these: the mutation tool does not mutate string literals,
 * so the GET_DUPLICATE_* queries themselves cannot be mutated. Assertions that
 * only exercise SQL filtering guard against hand edits but move no kill rate;
 * the ones that matter are on the C around them.
 * ---------------------------------------------------------------------------
 */

/* A scanned row with every column the whole-file loader ranks on. */
static int64_t put_dupe(struct dbhandle *db, const char *name, uint64_t ino,
			unsigned int dg, uint64_t size, unsigned int seq,
			unsigned int flags, unsigned int nr_extents)
{
	int64_t id = put_row(db, name, ino, 1, size, seq);
	unsigned char digest[DIGEST_LEN];

	digest_of(digest, dg);
	if (dbfile_update_scanned_file(db, id, digest, flags, nr_extents))
		abort();
	return id;
}

/*
 * The sole group in a results tree. abort()s rather than returning NULL when
 * there is not exactly one: that means the fixture is broken, and a test that
 * quietly took the first of two groups would pass while the loader split them.
 */
static struct dupe_extents *only_group(struct results_tree *res)
{
	if (res->num_dupes != 1 || RB_EMPTY_ROOT(&res->root))
		abort();
	return rb_entry(rb_first(&res->root), struct dupe_extents, de_node);
}

/*
 * The group's dedupe target: the loader orders it first, and
 * dedupe_extent_list() always dedupes against list_first_entry().
 *
 * abort()s on an empty member list, because list_first_entry() on one returns
 * a pointer derived from the list head rather than NULL - so a NULL check at
 * the call site would pass while dereferencing nothing.
 */
static const char *target_of(struct dupe_extents *d)
{
	if (list_empty(&d->de_extents))
		abort();
	return list_first_entry(&d->de_extents, struct extent, e_list)
		->e_file->filename;
}

/*
 * A whole-file duplicate group loads every member of the window at poff 0.
 *
 * That zero is not incidental. GET_DUPLICATE_FILES has no fiemap to draw on,
 * so whole-file members carry no physical offset - which is exactly why they
 * skip clean_deduped() and why `--dedupe-options=only_whole_files` converged
 * while the extent path did not (#186). A loader that invented a poff here
 * would put them back on the path that failed to converge.
 */
MU_TEST(test_whole_file_dupes_load_at_poff_zero) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct results_tree res;
	struct dupe_extents *d;
	struct extent *e;
	unsigned int n = 0;

	free_all_filerecs();
	init_results_tree(&res);

	/* Three copies of one 8 KiB file, all scanned in this window. The
	 * tidiest is deliberately not the first inserted, so "target first" is
	 * a claim about the election rather than about insertion order. */
	put_dupe(db, "/tree/a", 1, 7, 8192, 1, 0, 4);
	put_dupe(db, "/tree/b", 2, 7, 8192, 1, 0, 1);
	put_dupe(db, "/tree/c", 3, 7, 8192, 1, 0, 6);
	/* A file of the same size but a different digest is not a duplicate. */
	put_dupe(db, "/tree/d", 4, 8, 8192, 1, 0, 1);
	/* ...and neither is one of the same digest at a different size. */
	put_dupe(db, "/tree/e", 5, 7, 4096, 1, 0, 1);

	mu_check(dbfile_load_same_files(db, &res, 0, 1) == 0);

	mu_check(res.num_dupes == 1);
	d = only_group(&res);
	mu_check(d->de_num_dupes == 3);
	mu_check(d->de_len == 8192);
	mu_assert_string_eq("/tree/b", target_of(d));	/* ordered first */

	list_for_each_entry(e, &d->de_extents, e_list) {
		mu_check(e->e_loff == 0);
		mu_check(e->e_poff == 0);	/* no fiemap here, and #186 */
		n++;
	}
	mu_check(n == 3);

	free_results_tree(&res);
	free_all_filerecs();
}

/* Load one window over three same-size copies with the given per-file
 * (flags, nr_extents), and report which one the query elected. Names are
 * assigned in the order given, so a caller can put a low id anywhere. */
static const char *elected_target(const char *const names[3],
				  const unsigned int flags[3],
				  const unsigned int nr_extents[3])
{
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	static char winner[64];
	struct results_tree res;

	free_all_filerecs();
	init_results_tree(&res);
	for (unsigned int i = 0; i < 3; i++)
		put_dupe(db, names[i], i + 1, 7, 8192, 1, flags[i],
			 nr_extents[i]);
	if (dbfile_load_same_files(db, &res, 0, 1))
		abort();
	snprintf(winner, sizeof(winner), "%s", target_of(only_group(&res)));
	free_results_tree(&res);
	free_all_filerecs();
	return winner;
}

/*
 * Which member becomes the target, one ranking level per case.
 *
 * A read-only member wins outright: the kernel will not write into one, so it
 * has to be the source rather than a destination (#172). Below that the
 * least-fragmented member wins, because every copy inherits the target's
 * fragmentation. Both are read from columns fixed at scan time rather than
 * probed live, so every generation window reaches the same answer (#197).
 */
MU_TEST(test_the_whole_file_target_prefers_readonly_then_fewest_extents) {
	static const char *const abc[3] = { "/tree/a", "/tree/b", "/tree/c" };
	/* Ids ascend with position, so putting z first gives it the lowest. */
	static const char *const zyx[3] = { "/tree/z", "/tree/y", "/tree/x" };
	static const unsigned int none[3] = { 0, 0, 0 };

	/* Fewest extents wins, and the winner is neither first nor lowest id. */
	{
		static const unsigned int nr[3] = { 9, 2, 5 };

		mu_assert_string_eq("/tree/b", elected_target(abc, none, nr));
	}

	/* A read-only member wins even when it is both later and worse. */
	{
		static const unsigned int ro[3] = { 0, 0, FILE_RO_SUBVOL };
		static const unsigned int nr[3] = { 1, 3, 9 };

		mu_assert_string_eq("/tree/c", elected_target(abc, ro, nr));
	}

	/*
	 * Everything else equal, the lowest id breaks the tie - and the names
	 * descend while the ids ascend, so an ordering that fell back to the
	 * filename, or to no tie-break at all, would answer differently.
	 */
	{
		static const unsigned int nr[3] = { 4, 4, 4 };

		mu_assert_string_eq("/tree/z", elected_target(zyx, none, nr));
	}
}

/*
 * #197 itself: two overlapping generation windows must elect the *same*
 * target, and the election ranges over every member of the group rather than
 * the window's.
 *
 * The fixture is built so that weaker elections give different answers. The
 * winner (`old2`) is neither the lowest id nor a member of either window, so
 * an election reduced to min(id) - roughly the pre-#197 behaviour - answers
 * `old1`, and a window-local one answers a member of the window. A fixture
 * whose winner happened to be lowest-id *and* tidiest *and* oldest would pass
 * against all three, which is what the first draft of this test did.
 */
MU_TEST(test_every_window_elects_the_same_whole_file_target) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct results_tree first, second;
	const char *t1, *t2;

	free_all_filerecs();

	/* id 1 is old and ragged; id 2 is old and tidiest - the winner. */
	put_dupe(db, "/tree/old1", 1, 7, 8192, 1, 0, 6);
	put_dupe(db, "/tree/old2", 2, 7, 8192, 1, 0, 1);
	put_dupe(db, "/tree/mid", 3, 7, 8192, 2, 0, 5);
	put_dupe(db, "/tree/new", 4, 7, 8192, 3, 0, 4);

	init_results_tree(&first);
	mu_check(dbfile_load_same_files(db, &first, 1, 2) == 0);
	t1 = target_of(only_group(&first));

	init_results_tree(&second);
	mu_check(dbfile_load_same_files(db, &second, 2, 3) == 0);
	t2 = target_of(only_group(&second));

	mu_assert_string_eq("/tree/old2", t1);
	mu_assert_string_eq("/tree/old2", t2);

	/* Each window loads its own new member plus that target, and nothing
	 * else: two members, not four. */
	mu_check(only_group(&first)->de_num_dupes == 2);
	mu_check(only_group(&second)->de_num_dupes == 2);

	/*
	 * The loader marks the group anchored, from the query's is_target
	 * column. Nothing reads the flag today (#237), which is precisely why
	 * it is asserted here: without this the one C statement in
	 * dbfile_load_same_files() that exists for #197 can be deleted with
	 * every other test still green.
	 */
	mu_check(only_group(&first)->de_anchored);
	mu_check(only_group(&second)->de_anchored);

	free_results_tree(&first);
	free_results_tree(&second);
	free_all_filerecs();
}

/*
 * An inlined file is not a group member, not a target, and never becomes a
 * filerec at all. oans stores no extents for one and the kernel will not
 * deduplicate it, so a group formed around one is work that can only fail.
 */
MU_TEST(test_an_inlined_file_is_never_loaded_as_a_duplicate) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct results_tree res;
	struct dupe_extents *d;
	struct extent *e;
	int64_t inlined;

	free_all_filerecs();
	init_results_tree(&res);

	/* Two real copies and one inlined, all the same digest and size. */
	put_dupe(db, "/tree/a", 1, 7, 8192, 1, 0, 1);
	put_dupe(db, "/tree/b", 2, 7, 8192, 1, 0, 1);
	inlined = put_dupe(db, "/tree/inline", 3, 7, 8192, 1, FILE_INLINED, 0);

	mu_check(dbfile_load_same_files(db, &res, 0, 1) == 0);

	d = only_group(&res);
	mu_check(d->de_num_dupes == 2);
	list_for_each_entry(e, &d->de_extents, e_list)
		mu_check(strcmp(e->e_file->filename, "/tree/inline") != 0);

	/* Not merely absent from the group - never constructed. The loader
	 * creates a filerec per row it accepts, so this is an assertion about
	 * code that ran rather than about rows the query withheld. */
	mu_check(filerec_find(inlined) == NULL);

	free_results_tree(&res);
	free_all_filerecs();
}

/*
 * dbfile_load_one_filerec() answers "no such file" with success and a NULL
 * out-parameter, not an error. The dedupe phase calls it for ids it read from
 * rows that may since have been pruned, so a missing file is ordinary; a
 * loader returning an error there would abort a whole batch over a file
 * somebody deleted mid-run.
 */
MU_TEST(test_loading_one_filerec_treats_a_missing_id_as_success) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct filerec *f = NULL;
	int64_t id;

	free_all_filerecs();
	id = put_dupe(db, "/tree/only", 1, 7, 12288, 1, 0, 1);

	mu_check(dbfile_load_one_filerec(db, id, &f) == 0);
	mu_check(f != NULL);
	mu_assert_string_eq("/tree/only", f->filename);
	mu_check(f->fileid == id);
	mu_check(f->size == 12288);		/* the row's size, not a default */

	/* Absent: success, and the caller's pointer cleared rather than left
	 * holding whatever it had. */
	f = (struct filerec *)(uintptr_t)0x1;
	mu_check(dbfile_load_one_filerec(db, id + 999, &f) == 0);
	mu_check(f == NULL);

	free_all_filerecs();
}

/*
 * Block hashes load into a hash tree, and come out of it sorted.
 *
 * The loader ends with sort_file_hash_heads() because find_dupes walks each
 * file's blocks under a hash expecting increasing offsets. GET_DUPLICATE_BLOCKS
 * orders only by (dedupe_seq > ?1), fileid, and add_file_hash_head() appends in
 * arrival order - so rows genuinely arrive jumbled and this is the one place
 * that ordering is established for the dedupe phase.
 */
MU_TEST(test_block_hashes_load_into_the_tree_in_offset_order) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct hash_tree tree;
	unsigned char dg[DIGEST_LEN];
	struct block_csum blocks[3];
	struct dupe_blocks_list *dl;
	struct rb_node *n;
	unsigned int heads = 0;
	int64_t a, b;

	free_all_filerecs();
	init_hash_tree(&tree);
	digest_of(dg, 3);

	a = put_dupe(db, "/tree/a", 1, 7, 12288, 1, 0, 1);
	b = put_dupe(db, "/tree/b", 2, 8, 12288, 1, 0, 1);

	/* Deliberately not ascending. */
	blocks[0].loff = 8192;
	blocks[1].loff = 0;
	blocks[2].loff = 4096;
	for (unsigned int i = 0; i < 3; i++)
		memcpy(blocks[i].digest, dg, DIGEST_LEN);
	mu_check(dbfile_store_block_hashes(db, a, 3, blocks) == 0);
	mu_check(dbfile_store_block_hashes(db, b, 3, blocks) == 0);

	mu_check(dbfile_load_block_hashes(db, &tree, 0, 1) == 0);

	mu_check(tree.num_blocks == 6);
	mu_check(tree.num_hashes == 1);
	dl = find_block_list(&tree, dg);
	mu_check(dl != NULL);
	mu_check(dl->dl_num_elem == 6);

	/* Both files present, each holding exactly 0, 4096, 8192 in order. */
	for (n = rb_first(&dl->dl_files_root); n; n = rb_next(n)) {
		struct file_hash_head *h =
			rb_entry(n, struct file_hash_head, h_node);
		struct file_block *blk;
		unsigned int i = 0;

		heads++;
		list_for_each_entry(blk, &h->h_blocks, b_head_list) {
			mu_check(blk->b_loff == i * 4096);
			i++;
		}
		mu_check(i == 3);
	}
	mu_check(heads == 2);

	free_hash_tree(&tree);
	free_all_filerecs();
}

/*
 * Extent hashes load as a group per (digest, length), carrying the physical
 * offset the scan recorded - which is what the extent path needs and the
 * whole-file path deliberately lacks.
 *
 * A group needs two members: one extent with a digest nothing else shares is
 * not a duplicate of anything, and loading it would put a group into the tree
 * that the worker can only throw away.
 */
MU_TEST(test_extent_hashes_load_as_groups_carrying_their_offsets) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct results_tree res;
	struct dupe_extents *d;
	struct extent *e;
	unsigned char shared[DIGEST_LEN], lonely[DIGEST_LEN];
	struct extent_csum ea[2], eb[1];
	int64_t a, b;
	unsigned int seen = 0;

	free_all_filerecs();
	init_results_tree(&res);
	digest_of(shared, 4);
	digest_of(lonely, 5);

	a = put_dupe(db, "/tree/a", 1, 7, 65536, 1, 0, 2);
	b = put_dupe(db, "/tree/b", 2, 8, 65536, 1, 0, 2);

	/* One extent shared between the files, one only file a has. */
	ea[0].loff = 0;
	ea[0].poff = 4096;
	ea[0].len = 4096;
	memcpy(ea[0].digest, shared, DIGEST_LEN);
	ea[1].loff = 4096;
	ea[1].poff = 8192;
	ea[1].len = 4096;
	memcpy(ea[1].digest, lonely, DIGEST_LEN);
	eb[0].loff = 16384;
	eb[0].poff = 65536;
	eb[0].len = 4096;
	memcpy(eb[0].digest, shared, DIGEST_LEN);

	mu_check(dbfile_store_extent_hashes(db, a, 2, ea) == 0);
	mu_check(dbfile_store_extent_hashes(db, b, 1, eb) == 0);

	mu_check(dbfile_load_extent_hashes(db, &res, 0, 1) == 0);

	/* Only the shared digest makes a group. */
	mu_check(res.num_extents == 2);
	d = only_group(&res);
	mu_check(d->de_len == 4096);
	mu_check(!memcmp(d->de_hash, shared, DIGEST_LEN));

	/*
	 * Never anchored: only the whole-file loader elects a target, because
	 * only there do the members have differing layouts to rank. The pair
	 * with the assertion in the #197 test above is what pins that
	 * asymmetry rather than each half separately looking arbitrary.
	 */
	mu_check(!d->de_anchored);

	/* Each member keeps the physical offset its row recorded - the extent
	 * path reads it to decide what is already shared. */
	list_for_each_entry(e, &d->de_extents, e_list) {
		if (e->e_loff == 0)
			mu_check(e->e_poff == 4096);
		else if (e->e_loff == 16384)
			mu_check(e->e_poff == 65536);
		else
			mu_fail("an extent at an offset nobody stored");
		seen++;
	}
	mu_check(seen == 2);

	free_results_tree(&res);
	free_all_filerecs();
}

MU_TEST(test_dedupe_classify_probe)
{
	/* Dispatched, and the destination was processed: SAME or DIFFERS. */
	mu_assert_int_eq(DEDUPE_SUPPORT_YES, dedupe_classify_probe(0, 0, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_YES, dedupe_classify_probe(0, 0, 1));
	/* errno is meaningless on success and must not be consulted. */
	mu_assert_int_eq(DEDUPE_SUPPORT_YES,
			 dedupe_classify_probe(0, EINVAL, 0));

	/* A stacking filesystem puts the lower filesystem's refusal in status
	 * and leaves rc/errno clean - the case dedupe_probe_fd documents. */
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(0, 0, -EINVAL));
	/* A status that does name the filesystem is still an answer. */
	mu_assert_int_eq(DEDUPE_SUPPORT_NO,
			 dedupe_classify_probe(0, 0, -EOPNOTSUPP));
	mu_assert_int_eq(DEDUPE_SUPPORT_NO,
			 dedupe_classify_probe(0, 0, -ENOTTY));

	/* Answers about the filesystem from the ioctl itself. EOPNOTSUPP is
	 * what ext4 returns (measured); ENOTTY is a kernel that does not know
	 * the ioctl. status is not filled in when the ioctl fails. */
	mu_assert_int_eq(DEDUPE_SUPPORT_NO,
			 dedupe_classify_probe(-1, EOPNOTSUPP, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_NO,
			 dedupe_classify_probe(-1, ENOTTY, 0));

	/*
	 * Answers about this file or this caller. EINVAL is the important one:
	 * it is documented for "the filesystem does not support deduplicating
	 * the ranges of the given files" *and* for ordinary per-file
	 * conditions, so reading it as NO would condemn a filesystem on the
	 * evidence of one awkward file.
	 */
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EINVAL, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EACCES, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EROFS, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EPERM, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EISDIR, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EBADF, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, ETXTBSY, 0));
}

MU_TEST_SUITE(test_suite) {
	MU_RUN_TEST(test_running_checksum_survives_save_restore);
	MU_RUN_TEST(test_running_checksum_repoints_the_secret_on_restore);
	MU_RUN_TEST(test_running_checksum_save_refuses_a_short_buffer);
	MU_RUN_TEST(test_running_checksum_rejects_foreign_state);
	MU_RUN_TEST(test_is_block_zeroed);
	MU_RUN_TEST(test_block_len);
	MU_RUN_TEST(test_is_file_renamed);
	MU_RUN_TEST(test_seen_inode);
	MU_RUN_TEST(test_get_extent);
	MU_RUN_TEST(test_fiemap_layout_key);
	MU_RUN_TEST(test_fiemap_maps_share);
	MU_RUN_TEST(test_fiemap_unshared_bytes);
	MU_RUN_TEST(test_fiemap_unshared_bytes_accumulates);
	MU_RUN_TEST(test_fiemap_phys_set_grows);
	MU_RUN_TEST(test_sanitize_ctrl);
	MU_RUN_TEST(test_progress_copy_path);
	MU_RUN_TEST(test_progress_path_two_stage_render);
	MU_RUN_TEST(test_storage_recommend_io_threads);
	MU_RUN_TEST(test_scan_bucket);
	MU_RUN_TEST(test_scan_workq_priority);
	MU_RUN_TEST(test_starved_worker_line_reads_idle);
	MU_RUN_TEST(test_scan_eta);
	MU_RUN_TEST(test_group_u64);
	MU_RUN_TEST(test_parse_size);
	MU_RUN_TEST(test_human_size);
	MU_RUN_TEST(test_human_duration);
	MU_RUN_TEST(test_num_digits);
	MU_RUN_TEST(test_longpath);
	MU_RUN_TEST(test_glob_basename);
	MU_RUN_TEST(test_glob_anchored_vs_any_depth);
	MU_RUN_TEST(test_glob_wildcards_respect_separators);
	MU_RUN_TEST(test_glob_double_star_without_a_separator);
	MU_RUN_TEST(test_glob_backslash_escapes);
	MU_RUN_TEST(test_glob_character_classes);
	MU_RUN_TEST(test_glob_directory_only);
	MU_RUN_TEST(test_glob_literal_paths_are_not_globs);
	MU_RUN_TEST(test_glob_reports_matching_pattern_and_counts);
	MU_RUN_TEST(test_glob_rejects_malformed);
	MU_RUN_TEST(test_glob_empty_set_matches_nothing);
	MU_RUN_TEST(test_prop_a_layout_matches_only_itself);
	MU_RUN_TEST(test_prop_a_split_record_is_not_the_layout_it_covers);
	MU_RUN_TEST(test_prop_stored_extents_come_back_where_they_were_put);
	MU_RUN_TEST(test_filerec_the_registry_finds_what_was_put_in_it);
	MU_RUN_TEST(test_filerec_is_unfindable_once_its_last_reference_goes);
	MU_RUN_TEST(test_prop_every_filerec_is_findable_by_its_own_id);
	MU_RUN_TEST(test_prop_the_hash_tree_counts_what_it_holds);
	MU_RUN_TEST(test_prop_removing_every_block_empties_the_hash_tree);
	MU_RUN_TEST(test_prop_sorting_puts_every_hash_head_in_offset_order);
	MU_RUN_TEST(test_prop_a_dup_group_counts_its_own_members);
	MU_RUN_TEST(test_removing_an_extent_collapses_a_group_of_one);
	MU_RUN_TEST(test_a_group_is_keyed_on_digest_and_length_together);
	MU_RUN_TEST(test_the_anchor_flag_latches_once_claimed);
	MU_RUN_TEST(test_insert_result_files_a_pair_under_one_length);
	MU_RUN_TEST(test_filerec_holds_one_descriptor_however_often_it_is_opened);
	MU_RUN_TEST(test_filerec_open_once_opens_each_file_exactly_once);
	MU_RUN_TEST(test_prop_a_filerec_token_is_found_by_its_filerec);
	MU_RUN_TEST(test_whole_file_dupes_load_at_poff_zero);
	MU_RUN_TEST(test_the_whole_file_target_prefers_readonly_then_fewest_extents);
	MU_RUN_TEST(test_every_window_elects_the_same_whole_file_target);
	MU_RUN_TEST(test_an_inlined_file_is_never_loaded_as_a_duplicate);
	MU_RUN_TEST(test_loading_one_filerec_treats_a_missing_id_as_success);
	MU_RUN_TEST(test_block_hashes_load_into_the_tree_in_offset_order);
	MU_RUN_TEST(test_extent_hashes_load_as_groups_carrying_their_offsets);
	MU_RUN_TEST(test_dedupe_classify_probe);

	/* The hashfile, against an in-memory SQLite - the same path a run
	 * without --hashfile takes, so none of this is a test-only seam. */
	MU_RUN_TEST(test_dbfile_files_are_unique_on_the_inode_pair);
	MU_RUN_TEST(test_dbfile_pruning_a_deleted_file_takes_its_hashes);
	MU_RUN_TEST(test_dbfile_advancing_the_generation_keeps_hashes_and_checkpoint);
	MU_RUN_TEST(test_dbfile_pruning_unscanned_files_spares_checkpointed_ones);
	MU_RUN_TEST(test_dbfile_scan_config_round_trips_and_coerces_the_old_auto);
	MU_RUN_TEST(test_dbfile_run_history_totals_accumulate_but_skips_are_the_last_run);
	MU_RUN_TEST(test_dbfile_layout_matches_compares_every_record);
	MU_RUN_TEST(test_dbfile_copying_a_donor_brings_all_three);
	MU_RUN_TEST(test_dbfile_a_stored_file_round_trips_every_field);
	MU_RUN_TEST(test_dbfile_hashes_round_trip_their_offsets);
	MU_RUN_TEST(test_dbfile_load_checkpoint_declines_what_it_cannot_vouch_for);
	MU_RUN_TEST(test_dbfile_pruning_keeps_what_still_exists);
	MU_RUN_TEST(test_dbfile_pruning_grows_past_its_initial_capacity);

	/* Property-based (src/proptest.h). Grouped rather than interleaved: they
	 * are a different question about the same code, and a failure here names
	 * a seed to replay rather than a case to read. */
	MU_RUN_TEST(test_prop_sanitize_ctrl_leaves_nothing_dangerous);
	MU_RUN_TEST(test_prop_sanitize_ctrl_truncates_on_whole_escapes);
	MU_RUN_TEST(test_prop_sanitize_ctrl_is_identity_on_ordinary_names);
	MU_RUN_TEST(test_prop_checksum_resumes_at_any_split);
	MU_RUN_TEST(test_prop_checksum_refuses_any_damaged_header);
	MU_RUN_TEST(test_prop_maps_share_with_themselves);
	MU_RUN_TEST(test_prop_maps_never_share_without_a_real_address);
	MU_RUN_TEST(test_prop_shared_ranges_have_nothing_left_to_free);
	MU_RUN_TEST(test_prop_unshared_bytes_are_bounded_and_credited_once);
	MU_RUN_TEST(test_prop_a_bare_pattern_reads_only_the_basename);
	MU_RUN_TEST(test_prop_adding_a_pattern_never_unexcludes_a_path);
	MU_RUN_TEST(test_prop_a_directory_pattern_never_matches_a_file);
	MU_RUN_TEST(test_prop_a_literal_path_matches_itself_and_nothing_else);
	MU_RUN_TEST(test_prop_parse_size_scales_by_the_suffix);
	MU_RUN_TEST(test_prop_human_size_picks_a_real_unit);
	MU_RUN_TEST(test_prop_human_duration_reads_back);
	MU_RUN_TEST(test_prop_num_digits_matches_printf);
	MU_RUN_TEST(test_prop_group_u64_truncates_to_a_prefix);
}

int main(int argc [[maybe_unused]], char *argv[]) {
	exec_path = argv[0];
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return MU_EXIT_CODE;
}
