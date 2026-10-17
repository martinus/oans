#ifndef __FILE_FLAGS_H__
#define __FILE_FLAGS_H__

/* Fiemap flags that would cause us to mark the extent as undeduplicable */
#define FIEMAP_SKIP_FLAGS	(FIEMAP_EXTENT_DATA_INLINE|FIEMAP_EXTENT_UNWRITTEN)

/*
 * The following flags may be used in the hashfile.
 * Do not change the values recklessly.
 */
/* File is inlined. We store no extents nor hashes for it.
 * We should not try to deduplicate this file, won't work anyway.
 */
#define FILE_INLINED		0x0001

/*
 * File lives in a read-only btrfs subvolume, as of the scan that recorded it.
 * Stored so the dedupe phase can prefer such a file as a group's *source*
 * without probing the filesystem: the choice has to be identical in every
 * generation window or the windows disagree about the target (#197), and a
 * live probe is not (a subvolume can be flipped mid-run).
 */
#define FILE_RO_SUBVOL		0x0002

/*
 * The digest was computed by a binary that reads the data next to a
 * preallocated (UNWRITTEN) extent. Older ones filled a whole read buffer with
 * zeroes as soon as it touched such an extent, so two different files could
 * get one digest (#273). A row without this bit is rechecked once: rehashed if
 * the file still has an UNWRITTEN extent, and just marked otherwise.
 */
#define FILE_UNWRITTEN_CHECKED	0x0004

/*
 * The extents were stored by a binary that keeps the row of the extent holding
 * EOF when a preallocated extent follows it. Older ones allowed for that
 * extent running past the file size only when it was FIEMAP_EXTENT_LAST, so it
 * got no row and the extent pass never deduped it. Rechecked once like
 * FILE_UNWRITTEN_CHECKED, and set together with it.
 */
#define FILE_EOF_EXTENT_CHECKED	0x0008

/*
 * The extent digests were computed by a binary that leaves a hole out of the
 * extent after it. Older ones hashed the zeroes of a hole that does not fill
 * whole hash blocks into the next extent's digest, so one extent behind holes
 * of different sizes got different digests and was never extent-deduped.
 * Rechecked once like FILE_UNWRITTEN_CHECKED, and set together with it.
 */
#define FILE_HOLE_EXTENT_CHECKED	0x0010

/* Every recheck bit: a row with all of them needs no fiemap. */
#define FILE_SCAN_CHECKED	(FILE_UNWRITTEN_CHECKED | FILE_EOF_EXTENT_CHECKED | \
				 FILE_HOLE_EXTENT_CHECKED)

#endif
