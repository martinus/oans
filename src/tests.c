#include <stdbool.h>
#include <sys/stat.h>

#include "minunit.h"
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
 * The FIDEDUPERANGE probe's error taxonomy (#224). Worth testing directly and
 * exhaustively: the probe's whole job is turning one errno into a verdict, and
 * which errno a filesystem returns is exactly what a test host cannot vary.
 * Both wrong answers are bad in different ways -- a wrong NO silently skips a
 * whole tree, a wrong YES brings back per-file dedupe errors -- so anything
 * that is not a clear statement about the filesystem must stay UNKNOWN.
 */
MU_TEST(test_dedupe_classify_probe)
{
	/* The ioctl went through: the filesystem implements it. */
	mu_assert_int_eq(DEDUPE_SUPPORT_YES, dedupe_classify_probe(0, 0));
	/* errno is meaningless on success and must not be consulted. */
	mu_assert_int_eq(DEDUPE_SUPPORT_YES, dedupe_classify_probe(0, EINVAL));

	/* Answers about the filesystem. EOPNOTSUPP is what ext4 returns
	 * (measured); ENOTTY is a kernel that does not know the ioctl. */
	mu_assert_int_eq(DEDUPE_SUPPORT_NO,
			 dedupe_classify_probe(-1, EOPNOTSUPP));
	mu_assert_int_eq(DEDUPE_SUPPORT_NO, dedupe_classify_probe(-1, ENOTTY));

	/*
	 * Answers about this file or this caller. EINVAL is the important one:
	 * it is documented for "the filesystem does not support deduplicating
	 * the ranges of the given files" *and* for ordinary per-file
	 * conditions, so reading it as NO would condemn a filesystem on the
	 * evidence of one awkward file.
	 */
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EINVAL));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EACCES));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EROFS));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EPERM));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EISDIR));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EBADF));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, ETXTBSY));
}

MU_TEST_SUITE(test_suite) {
	MU_RUN_TEST(test_running_checksum_survives_save_restore);
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
	MU_RUN_TEST(test_longpath);
	MU_RUN_TEST(test_glob_basename);
	MU_RUN_TEST(test_glob_anchored_vs_any_depth);
	MU_RUN_TEST(test_glob_wildcards_respect_separators);
	MU_RUN_TEST(test_glob_character_classes);
	MU_RUN_TEST(test_glob_directory_only);
	MU_RUN_TEST(test_glob_literal_paths_are_not_globs);
	MU_RUN_TEST(test_glob_reports_matching_pattern_and_counts);
	MU_RUN_TEST(test_glob_rejects_malformed);
	MU_RUN_TEST(test_glob_empty_set_matches_nothing);
	MU_RUN_TEST(test_dedupe_classify_probe);
}

int main(int argc [[maybe_unused]], char *argv[]) {
	exec_path = argv[0];
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return MU_EXIT_CODE;
}
