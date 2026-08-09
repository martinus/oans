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

#endif
