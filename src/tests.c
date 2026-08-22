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
	/* A C1 costs two input bytes and is named by its code point. */
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\x9f", &cp) == 2 && cp == 0x9f);
	/* 0xc2 not followed by a continuation byte is ordinary UTF-8 lead. */
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\xa9", &cp) == 0);

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

	/* Truncation stays NUL-terminated and within bounds. */
	char small[4];
	group_u64_snprintf(2505166, small, sizeof(small));
	mu_check(small[3] == '\0' && strlen(small) <= 3);
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
		/* An escape is spelled in characters a terminal cannot act on
		 * either, so the result is plain printable ASCII throughout. */
		for (const unsigned char *q = (const unsigned char *)out; *q; q++)
			prop_check(&p, *q >= 0x20 || *q == '\t' || *q == '\n');
	}
}

/*
 * `path_for_display` sizes its buffer at SANITIZE_CTRL_MAX per input byte, so
 * that bound is load-bearing rather than decorative: an encoding that expanded
 * further would overflow a heap allocation on the first crafted name.
 */
MU_TEST(test_prop_sanitize_ctrl_stays_inside_its_own_bound) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char in[24], out[SANITIZE_CTRL_MAX * 24 + 1];

		gen_hostile_name(&p, in, sizeof(in));
		sanitize_ctrl(in, out, sizeof(out));
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
		struct { char before[8]; char out[32]; char after[8]; } fenced;
		size_t sz = (size_t)prop_below(&p, sizeof(fenced.out) + 1);

		gen_hostile_name(&p, in, sizeof(in));
		memset(&fenced, '#', sizeof(fenced));
		sanitize_ctrl(in, fenced.out, sz);
		sanitize_ctrl(in, full, sizeof(full));

		for (size_t i = 0; i < sizeof(fenced.before); i++)
			prop_check(&p, fenced.before[i] == '#');
		for (size_t i = 0; i < sizeof(fenced.after); i++)
			prop_check(&p, fenced.after[i] == '#');
		if (sz == 0) {
			/* Nothing may be written at all, not even a NUL. */
			prop_check(&p, fenced.out[0] == '#');
			continue;
		}
		prop_check(&p, strlen(fenced.out) < sz);
		prop_check(&p, strncmp(fenced.out, full, strlen(fenced.out)) == 0);
		/* Truncated only when it had to be. */
		if (strlen(full) < sz)
			prop_check(&p, strcmp(fenced.out, full) == 0);
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

	while (prop_next(&p)) {
		unsigned char whole[DIGEST_LEN], resumed[DIGEST_LEN];
		size_t len = (size_t)prop_below(&p, sizeof(data) + 1);
		unsigned int splits = (unsigned int)prop_below(&p, 4);
		struct running_checksum *c;
		size_t at = 0;

		prop_bytes(&p, data, len);

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

static bool layout_has_phys(const struct fm_rec *r, unsigned int n)
{
	const uint32_t no_phys = FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC |
				 FIEMAP_EXTENT_DATA_INLINE;

	for (unsigned int i = 0; i < n; i++)
		if (r[i].flags & no_phys)
			return false;
	return true;
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
			if (dst[nd - 1].len > PROP_BLOCK && nd < PROP_MAX_RECS) {
				dst[nd].log = dst[nd - 1].log + PROP_BLOCK;
				dst[nd].phys = dst[nd - 1].phys;
				dst[nd].len = dst[nd - 1].len - PROP_BLOCK;
				dst[nd].flags = dst[nd - 1].flags;
				dst[nd - 1].len = PROP_BLOCK;
				dst[nd - 1].flags &= ~(uint32_t)FIEMAP_EXTENT_LAST;
				nd++;
			}
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
		switch (prop_below(p, 6)) {
		case 0: buf[i] = '*'; break;
		case 1: buf[i] = '?'; break;
		case 2: buf[i] = '.'; break;
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
		struct glob_set *gs = glob_set_new();
		char *err = NULL;
		bool is_dir = prop_bool(&p);

		gen_glob_path(&p, lit, sizeof(lit));
		/* Metacharacters in the *literal*, which is the case it exists
		 * for: as a pattern this would be a class or a wildcard. */
		if (prop_bool(&p)) {
			size_t n = strlen(lit);

			if (n + 3 < sizeof(lit)) {
				lit[n] = prop_bool(&p) ? '[' : '*';
				lit[n + 1] = 'a';
				lit[n + 2] = '\0';
			}
		}
		gen_glob_path(&p, other, sizeof(other));

		glob_set_add_literal(gs, lit);
		if (glob_set_compile(gs, &err)) {
			g_free(err);
			glob_set_free(gs);
			prop_check(&p, false);	/* a literal cannot be malformed */
		}
		prop_check(&p, glob_set_match(gs, lit, is_dir, NULL));
		if (strcmp(lit, other))
			prop_check(&p, !glob_set_match(gs, other, is_dir, NULL));
		glob_set_free(gs);
	}
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

	/* Property-based (src/proptest.h). Grouped rather than interleaved: they
	 * are a different question about the same code, and a failure here names
	 * a seed to replay rather than a case to read. */
	MU_RUN_TEST(test_prop_sanitize_ctrl_leaves_nothing_dangerous);
	MU_RUN_TEST(test_prop_sanitize_ctrl_stays_inside_its_own_bound);
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
}

int main(int argc [[maybe_unused]], char *argv[]) {
	exec_path = argv[0];
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return MU_EXIT_CODE;
}
