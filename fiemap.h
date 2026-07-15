#ifndef	__FIEMAP_H__
#define	__FIEMAP_H__

#include <linux/fiemap.h>
#include <sys/types.h>
#include <stdint.h>

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
#endif	/* __FIEMAP_H__ */
