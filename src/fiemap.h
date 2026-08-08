#ifndef	__FIEMAP_H__
#define	__FIEMAP_H__

#include <linux/fiemap.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Given a filled fiemap structure, extract the struct fiemap_extent
 * which covers the loff offset.
 * If index is not NULL, then it will be filled with the extent's index.
 * If no extent is found, returns NULL and index is garbage.
 * The returned value must not be used after fiemap is freed, and must not
 * be freed directly either.
 */
struct fiemap_extent *get_extent(struct fiemap *fiemap, size_t loff,
				 unsigned int *index);

/*
 * Extract the extents mapping of a file.
 * May not return all extents if the file changed while this function is
 * running.
 */
struct fiemap *do_fiemap(int fd);

/*
 * Extract the extent mapping for a specific byte range. Only returns extents
 * overlapping [start, start+length). Returns NULL on error OR when the range
 * maps no extents (a hole/unwritten prealloc) - unlike do_fiemap(), which hands
 * back a valid empty map - so callers must not treat NULL as strictly an error.
 */
struct fiemap *do_fiemap_range(int fd, uint64_t start, uint64_t length);

/*
 * Physical offset of the first extent overlapping [start, start+length), via a
 * single fiemap ioctl. Returns 0 and sets *poff on success, -1 on error or when
 * the range maps no extents.
 */
int fiemap_first_extent_poff(int fd, uint64_t start, uint64_t length,
			     uint64_t *poff);

/*
 * Count how much of the area between start_off and end_off is shared.
 */
int fiemap_count_shared(int fd, size_t start_off, size_t end_off, uint64_t *shared);

/*
 * True if [dest_off, dest_off+len) described by `dm` already resolves to the
 * same stored extents as [tgt_off, tgt_off+len) described by `tgt` - i.e. the
 * two ranges share all their storage, so deduping them would be a byte-for-byte
 * no-op. Both maps come from do_fiemap_range(); a caller comparing one target
 * against many destinations fetches the target's once.
 *
 * Compares coverage, not extent records: the same storage may be described
 * with different record boundaries in each file. Matching holes count as
 * shared. Conservative otherwise - returns false on any missing map, any
 * difference it cannot prove away, or any extent without a real physical
 * location - so a caller only ever skips a genuine no-op, never a real dedupe.
 *
 * `len` must be what the kernel would actually dedupe, i.e. run it through
 * dedupe_shareable_len() first. A trailing partial block is never shared, so
 * including one makes this permanently false for every unaligned file.
 */
bool fiemap_maps_share(const struct fiemap *tgt, uint64_t tgt_off,
		       const struct fiemap *dm, uint64_t dest_off, uint64_t len);

/*
 * The physical addresses a group has already accounted for, sorted and unique.
 * Seeded from the dedupe target's map, then grown by fiemap_unshared_bytes()
 * as each destination is measured, so storage two destinations already share
 * with each other is only ever credited once (#191).
 */
struct fiemap_phys_set {
	uint64_t	*v;
	unsigned int	n;
	unsigned int	cap;
};

void fiemap_phys_set_init(struct fiemap_phys_set *set, const struct fiemap *fm);
void fiemap_phys_set_free(struct fiemap_phys_set *set);

/*
 * Bytes of [dest_off, dest_off+len) not already on storage `seen` accounts
 * for - what deduplicating this destination would actually stop duplicating,
 * as opposed to what the kernel reports having compared. Adds the
 * destination's own addresses to `seen`. See the definition for where it
 * approximates; it must only ever feed a reported figure, never a decision to
 * skip work.
 */
uint64_t fiemap_unshared_bytes(struct fiemap_phys_set *seen,
			       const struct fiemap *dm, uint64_t dest_off,
			       uint64_t len);

/*
 * Number of physical extents overlapping [start, start+length), via a single
 * empty fiemap ioctl (no per-extent buffer). Pass start=0, length=~0ULL for the
 * whole file. Returns 0 on error.
 */
unsigned int fiemap_count_extents(int fd, uint64_t start, uint64_t length);
#endif	/* __FIEMAP_H__ */
