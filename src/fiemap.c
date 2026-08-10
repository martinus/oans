/*
 * fiemap.c
 *
 * Abstract and add helpers to the fiemap ioctl.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "csum.h"
#include "debug.h"
#include "fiemap.h"
#include "util.h"

/*
 * Empty fiemap ioctl to count the extents overlapping [start, start+length).
 * Pass start=0, length=~0ULL for the whole file. Returns 0 on error.
 */
unsigned int fiemap_count_extents(int fd, uint64_t start,
				  uint64_t length)
{
	struct fiemap fiemap = {0,};
	int err;

	fiemap.fm_start = start;
	fiemap.fm_length = length;

	err = ioctl(fd, FS_IOC_FIEMAP, &fiemap);
	if (err < 0) {
		perror("fiemap_count_extents");
		return 0;
	}

	return fiemap.fm_mapped_extents;
}

/*
 * Find the first extent whose range reaches loff (i.e. ends at or after it).
 *
 * `index` is an optional in/out resume cursor. Extents are sorted by logical
 * offset, so a caller that queries monotonically increasing offsets (the scan
 * of one file) can pass the previous result's index to avoid rescanning from 0
 * every call, turning an O(extents^2) walk into O(extents). Starting at the
 * hint is safe whenever the previous extent ends at or before loff (so no
 * earlier extent could be the answer); that also covers loff landing in the
 * hole just before the pointed extent - the case where the scan resumes right
 * after skipping a hole. A stale hint pointing past loff fails the test and
 * falls back to a full scan. Either way the answer is identical to scanning
 * from 0.
 */
struct fiemap_extent *get_extent(struct fiemap *fiemap, size_t loff,
				 unsigned int *index)
{
	struct fiemap_extent *extent;
	size_t ext_end_off;
	unsigned int start = 0;

	if (index && *index < fiemap->fm_mapped_extents &&
	    (*index == 0 ||
	     fiemap->fm_extents[*index - 1].fe_logical +
	     fiemap->fm_extents[*index - 1].fe_length <= loff))
		start = *index;

	for (unsigned int i = start; i < fiemap->fm_mapped_extents; i++) {
		extent = &fiemap->fm_extents[i];
		ext_end_off = extent->fe_logical + extent->fe_length - 1;
		if (ext_end_off < loff)
			continue;

		if (index)
			*index = i;

		return extent;
	}
	return NULL;
}

/*
 * Map `count` extents of [start, start+length) into a freshly allocated fiemap.
 * `count` normally comes from a preceding fiemap_count_extents() pass. Returns
 * NULL on allocation or ioctl error.
 */
static struct fiemap *fiemap_map(int fd, uint64_t start, uint64_t length,
				 unsigned int count)
{
	struct fiemap *fiemap;

	/*
	 * The structure must be large enough to fit one struct fiemap plus
	 * $count struct fiemap_extent. We over-allocate a pointer per extent to
	 * match historical behaviour; it is harmless. See
	 * https://www.kernel.org/doc/Documentation/filesystems/fiemap.txt
	 */
	fiemap = calloc(1, sizeof(struct fiemap) +
			count * (sizeof(struct fiemap_extent) +
			sizeof(struct fiemap_extent *)));
	if (!fiemap)
		return NULL;

	fiemap->fm_start = start;
	fiemap->fm_length = length;
	fiemap->fm_extent_count = count;

	if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0) {
		perror("fiemap");
		free(fiemap);
		return NULL;
	}

	if (fiemap->fm_mapped_extents != count)
		dprintf("fiemap: file changed between fiemap calls\n");

	return fiemap;
}

struct fiemap *do_fiemap(int fd)
{
	return fiemap_map(fd, 0, ~0ULL, fiemap_count_extents(fd, 0, ~0ULL));
}

/*
 * Like do_fiemap() but only maps the [start, start+length) byte range. The
 * dedupe phase only needs the extent(s) at one offset, so this avoids
 * enumerating the whole (possibly huge/fragmented) file's extent map. Returns
 * NULL when the range maps no extents (a hole) as well as on error.
 */
struct fiemap *do_fiemap_range(int fd, uint64_t start, uint64_t length)
{
	unsigned int count = fiemap_count_extents(fd, start, length);

	if (count == 0)
		return NULL;
	return fiemap_map(fd, start, length, count);
}

/*
 * Physical offset of the first extent overlapping [start, start+length).
 *
 * The dedupe rescan only needs that one extent, so unlike do_fiemap_range()
 * this issues a single ioctl (no separate count pass) into a one-extent buffer.
 * Returns 0 and stores the offset in *poff on success, -1 on ioctl error or
 * when the range maps no extents (a hole).
 */
int fiemap_first_extent_poff(int fd, uint64_t start, uint64_t length,
			     uint64_t *poff)
{
	struct {
		struct fiemap		fiemap;
		struct fiemap_extent	extent;
	} buf = {0,};

	buf.fiemap.fm_start = start;
	buf.fiemap.fm_length = length;
	buf.fiemap.fm_extent_count = 1;

	if (ioctl(fd, FS_IOC_FIEMAP, &buf.fiemap) < 0) {
		perror("fiemap_first_extent_poff");
		return -1;
	}

	if (buf.fiemap.fm_mapped_extents == 0)
		return -1;

	*poff = buf.fiemap.fm_extents[0].fe_physical;
	return 0;
}

int fiemap_count_shared(int fd, size_t start_off, size_t end_off, uint64_t *shared)
{
	_cleanup_(freep) struct fiemap *fiemap = NULL;
	struct fiemap_extent *extent;

	size_t extent_loff;
	size_t extent_end;

	abort_on(start_off >= end_off);

	fiemap = do_fiemap_range(fd, start_off, end_off - start_off);
	if (!fiemap) {
		*shared = 0;
		return 0;
	}

	*shared = 0;

	for (unsigned int i = 0; i < fiemap->fm_mapped_extents; i++) {
		extent = &fiemap->fm_extents[i];

		extent_end = extent->fe_logical + extent->fe_length;
		extent_loff = extent->fe_logical;

		if (start_off <= extent_end && end_off >= extent_loff) {
			if (!(extent->fe_flags & FIEMAP_EXTENT_DELALLOC)
					&& extent->fe_flags & FIEMAP_EXTENT_SHARED) {
				if (extent_loff < start_off)
					extent_loff = start_off;
				if (end_off < extent_end)
					extent_end = end_off;
				*shared += extent_end - extent_loff;
			}
		}
	}
	return 0;
}

/* Extents whose fe_physical is not a real, stable on-disk location. */
#define FIEMAP_NO_PHYS (FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | \
			FIEMAP_EXTENT_DATA_INLINE)

/*
 * Unallocated bytes at `off`: fiemap omits holes, so a gap before the record -
 * or having run out of records - is one. `rest` is what remains of the range.
 */
static uint64_t gap_at(const struct fiemap_extent *e, uint64_t off, uint64_t rest)
{
	if (!e)
		return rest;
	return e->fe_logical > off ? e->fe_logical - off : 0;
}

/*
 * Do the two ranges already resolve to the same stored extents, so that
 * deduping the destination against the target would change nothing?
 *
 * `len` must already be what the kernel would actually dedupe (see
 * dedupe_shareable_len()); see fiemap.h.
 *
 * This walks the range instead of comparing extent records one for one,
 * because the same storage gets described with different record boundaries in
 * each file - the split tail a dedupe leaves behind is enough - and treating
 * that as "not shared" resubmits the whole file to the kernel on every run
 * (#186). Holes count as shared when both sides have one.
 *
 * fe_physical is only ever compared for equality, never with an offset added:
 * on a compressed extent it addresses the compressed extent as a whole, so
 * physical + logical_delta is meaningless. Extents with no real physical
 * location (delalloc, unknown, inline) all report fe_physical 0 and must never
 * count as shared.
 */
bool fiemap_maps_share(const struct fiemap *tgt, uint64_t tgt_off,
		       const struct fiemap *dm, uint64_t dest_off,
		       uint64_t len)
{
	unsigned int i = 0;	/* both maps advance together, or we bail */
	uint64_t pos = 0;	/* bytes of the range proven shared so far */

	if (len == 0 || !tgt || !dm || tgt->fm_mapped_extents == 0)
		return false;

	while (pos < len) {
		const struct fiemap_extent *ea, *eb;
		uint64_t pa, pb;	/* range position, relative to the record */
		uint64_t ha, hb;	/* unallocated bytes at this position */
		uint64_t ra, rb;

		ea = i < tgt->fm_mapped_extents ? &tgt->fm_extents[i] : NULL;
		eb = i < dm->fm_mapped_extents ? &dm->fm_extents[i] : NULL;

		ha = gap_at(ea, tgt_off + pos, len - pos);
		hb = gap_at(eb, dest_off + pos, len - pos);
		if (ha || hb) {
			/* A hole facing data is a real difference. */
			if (ha != hb)
				return false;
			pos += ha;
			continue;
		}

		if ((ea->fe_flags | eb->fe_flags) & FIEMAP_NO_PHYS)
			return false;

		/*
		 * Both sides must be on the same stored extent at the same
		 * offset into it. pa/pb are nonzero only for a first record
		 * that begins before the range.
		 */
		pa = tgt_off + pos - ea->fe_logical;
		pb = dest_off + pos - eb->fe_logical;
		if (pa != pb || ea->fe_physical != eb->fe_physical)
			return false;

		ra = ea->fe_length - pa;
		rb = eb->fe_length - pb;
		pos += ra < rb ? ra : rb;
		i++;

		/*
		 * Same storage, different record boundaries. The shorter side
		 * continues in its next record, but lining the maps back up
		 * would need the offset arithmetic ruled out above - so accept
		 * it only when the range is already covered, which is the split
		 * tail this walk exists for.
		 */
		if (ra != rb && pos < len)
			return false;
	}
	return true;
}

/* Sorted-unique in place; returns the surviving count. */
static unsigned int sort_uniq(uint64_t *v, unsigned int n)
{
	unsigned int w = 1;

	if (n < 2)
		return n;
	qsort(v, n, sizeof(*v), cmp_u64);
	for (unsigned int i = 1; i < n; i++)
		if (v[i] != v[w - 1])
			v[w++] = v[i];
	return w;
}

void fiemap_phys_set_init(struct fiemap_phys_set *set, const struct fiemap *fm)
{
	unsigned int n = 0;

	set->v = NULL;
	set->n = set->cap = 0;
	if (!fm || fm->fm_mapped_extents == 0)
		return;

	set->v = malloc(fm->fm_mapped_extents * sizeof(*set->v));
	if (!set->v)
		return;
	set->cap = fm->fm_mapped_extents;

	for (unsigned int i = 0; i < fm->fm_mapped_extents; i++) {
		if (fm->fm_extents[i].fe_flags & FIEMAP_NO_PHYS)
			continue;	/* no stable address to match against */
		set->v[n++] = fm->fm_extents[i].fe_physical;
	}
	/* Duplicates are common - a compressed extent reports one address for
	 * every record referencing it - and collapsing them shortens every
	 * later search. */
	set->n = sort_uniq(set->v, n);
}

void fiemap_phys_set_free(struct fiemap_phys_set *set)
{
	free(set->v);
	set->v = NULL;
	set->n = set->cap = 0;
}

/*
 * Merge `add` (sorted, unique) into the set. Merging per destination rather
 * than inserting per address keeps this O(n) per destination instead of O(n)
 * per element - the difference between usable and hopeless on the fragmented
 * whole-file groups the sorted array exists for.
 *
 * On allocation failure the set is left as it was: the reported figure loses
 * accuracy for the rest of the group, which is all this feeds.
 */
static void phys_set_merge(struct fiemap_phys_set *set, const uint64_t *add,
			   unsigned int n_add)
{
	unsigned int i, j, k, w;

	if (!n_add)
		return;

	if (set->n + n_add > set->cap) {
		unsigned int cap = set->cap ? set->cap : 16;
		uint64_t *grown;

		while (cap < set->n + n_add)
			cap *= 2;
		grown = realloc(set->v, cap * sizeof(*grown));
		if (!grown)
			return;
		set->v = grown;
		set->cap = cap;
	}

	/* Backwards, so the merge can run in place. */
	i = set->n;
	j = n_add;
	k = set->n + n_add;
	while (j > 0) {
		if (i > 0 && set->v[i - 1] > add[j - 1])
			set->v[--k] = set->v[--i];
		else
			set->v[--k] = add[--j];
	}
	set->n += n_add;

	w = 1;
	for (i = 1; i < set->n; i++)
		if (set->v[i] != set->v[w - 1])
			set->v[w++] = set->v[i];
	set->n = w;
}

/*
 * How many bytes of [dest_off, dest_off+len) are NOT already on storage that
 * `seen` accounts for - i.e. how much deduplicating this destination would
 * actually stop duplicating - after which its addresses join the set.
 *
 * The set accumulating is the point. It starts as the target's addresses, but
 * two destinations of one group can already share an extent with *each other*;
 * crediting both in full claimed twice what releasing that one extent frees
 * (#191). Whoever is measured first pays for it, and the rest are free.
 *
 * Membership is by physical address: two records with the same fe_physical are
 * the same stored bytes, so a destination already sitting on accounted-for
 * storage frees nothing, and where in the range it sits is irrelevant to that.
 * Holes contribute nothing, having nothing to free.
 *
 * Approximate at the edges, by at most one extent either way: a partial
 * reference at a different offset into the same uncompressed extent reports a
 * different fe_physical and is counted as unshared, while a compressed extent
 * reports one address for the whole extent so any part of it counts as shared.
 * One address repeated *within* a single destination is also still counted
 * twice. This only feeds the reported figure, never a decision to skip work.
 */
uint64_t fiemap_unshared_bytes(struct fiemap_phys_set *seen,
			       const struct fiemap *dm, uint64_t dest_off,
			       uint64_t len)
{
	const uint64_t dest_end = dest_off + len;
	_cleanup_(freep) uint64_t *fresh = NULL;
	unsigned int n_fresh = 0;
	uint64_t unshared = 0;

	/* No destination map means we could not tell: assume nothing is shared,
	 * which is what the figure said before it could tell at all. */
	if (!dm)
		return len;

	fresh = malloc(dm->fm_mapped_extents * sizeof(*fresh));

	for (unsigned int i = 0; i < dm->fm_mapped_extents; i++) {
		const struct fiemap_extent *e = &dm->fm_extents[i];
		uint64_t start = e->fe_logical > dest_off ? e->fe_logical : dest_off;
		uint64_t end = e->fe_logical + e->fe_length;
		bool addressable = !(e->fe_flags & FIEMAP_NO_PHYS);

		if (end > dest_end)
			end = dest_end;
		if (end <= start)
			continue;

		if (addressable && seen->n &&
		    bsearch(&e->fe_physical, seen->v, seen->n, sizeof(*seen->v),
			    cmp_u64))
			continue;	/* already accounted for */

		unshared += end - start;
		if (addressable && fresh)
			fresh[n_fresh++] = e->fe_physical;
	}

	if (fresh)
		phys_set_merge(seen, fresh, sort_uniq(fresh, n_fresh));
	return unshared;
}

/* Flags that make a record's physical address meaningless or unstable. */
#define LAYOUT_REJECT_FLAGS	(FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | \
				 FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_DATA_ENCRYPTED)
/* Flags that say nothing about content. */
#define LAYOUT_IGNORE_FLAGS	(FIEMAP_EXTENT_SHARED | FIEMAP_EXTENT_LAST)

bool fiemap_layout_key(const struct fiemap *fm, uint64_t size,
		       unsigned char *key)
{
	struct running_checksum *csum;
	uint64_t header[2];
	unsigned int i;

	if (!fm || fm->fm_mapped_extents == 0)
		return false;

	for (i = 0; i < fm->fm_mapped_extents; i++) {
		if (fm->fm_extents[i].fe_flags & LAYOUT_REJECT_FLAGS)
			return false;
	}

	csum = start_running_checksum();
	if (!csum)
		return false;

	/*
	 * Size and extent count go in first, so a layout that is a prefix of
	 * another cannot hash the same, and the size covers a trailing hole -
	 * which fiemap does not report at all.
	 */
	header[0] = size;
	header[1] = fm->fm_mapped_extents;
	add_to_running_checksum(csum, (unsigned char *)header, sizeof(header));

	for (i = 0; i < fm->fm_mapped_extents; i++) {
		const struct fiemap_extent *e = &fm->fm_extents[i];
		uint64_t rec[4] = {
			e->fe_logical, e->fe_physical, e->fe_length,
			e->fe_flags & ~(uint64_t)LAYOUT_IGNORE_FLAGS,
		};

		add_to_running_checksum(csum, (unsigned char *)rec, sizeof(rec));
	}

	finish_running_checksum(csum, key);
	return true;
}
