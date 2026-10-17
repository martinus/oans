/*
 * dedupe.h
 *
 * Copyright (C) 2016 SUSE.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */

#ifndef	__DEDUPE_H__
#define	__DEDUPE_H__

#include "list.h"
#include "ioctl.h"

#define MAX_DEDUPES_PER_IOCTL	120

struct dedupe_ctxt {

	/*
	 * Starting len/file off saved for the callers convenience -
	 * the ones below can change during dedupe operations.
	 */
	uint64_t	orig_len;
	uint64_t	orig_file_off;

	uint64_t	len;
	struct filerec	*ioctl_file;
	uint64_t	ioctl_file_off;

	/* Next two are used for sanity checking */
	unsigned int		max_queable;
	unsigned int		num_queued;

	unsigned int		same_size;

	/*
	 * This request's destination came back EINVAL once, so its lengths
	 * are rounded down to whole blocks from here on - for older kernels
	 * that reject an unaligned length rather than shorten it. Per request,
	 * not per process (#280): one EINVAL, say from a NODATASUM mismatch,
	 * used to shorten every later request on every thread, and so leave
	 * every later file's last partial block unshared for good.
	 */
	bool			aligned;

	/*
	 * request tracking.
	 *	queued: request is awaiting dedupe
	 *	in_progress: currently undergoing dedupe operations
	 *	completed: results of dedupe for this request are available
	 */
	struct list_head	queued;
	struct list_head	in_progress;
	struct list_head	completed;

	/*
	 * Optional live progress callback: when set, dedupe_extents() reports
	 * the bytes deduped after every ioctl round, so a status display can
	 * show movement inside large requests instead of one jump at the end.
	 * A callback (rather than a bare counter) keeps dedupe.c free of the
	 * progress.h layering: run_dedupe.c both ticks its per-thread status
	 * line and credits the global byte bar from the same callback.
	 */
	void			(*progress_fn)(void *arg, uint64_t bytes);
	void			*progress_arg;

	struct file_dedupe_range *same;
};

struct dedupe_ctxt *new_dedupe_ctxt(unsigned int max_extents, uint64_t loff,
				    uint64_t elen, struct filerec *ioctl_file);
void free_dedupe_ctxt(struct dedupe_ctxt *ctxt);

/*
 * How much of a `len`-byte dedupe request the kernel would actually share:
 * it rounds the length down to a whole filesystem block
 * (generic_remap_check_len()), so a trailing partial block never gets shared.
 * A range under one block is returned unchanged - there is nothing to round
 * down to, and the kernel takes it whole or not at all.
 *
 * The one rule, for both the code that submits requests and the code that
 * reasons about what a request would achieve.
 */
uint64_t dedupe_shareable_len(int fd, uint64_t len);

/* The filesystem's block size, queried once per process. */
unsigned int dedupe_blocksize(int fd);

/*
 * Queue one destination. `unshared` is how many of its bytes are not already
 * on the target's storage - what this dedupe would actually stop duplicating,
 * handed back by pop_one_dedupe_result() so the caller reports space freed
 * rather than bytes the kernel compared (#187). It must be measured against
 * the target (fiemap_unshared_bytes()); passing the full length credits the
 * whole range, which is that over-count, and is only right when the target
 * could not be mapped at all.
 *
 * Returns:
 *  < 0: error
 * == 0: no more extents after this one
 *  > 0: ok, can accept more extents
 */
int add_extent_to_dedupe(struct dedupe_ctxt *ctxt, uint64_t loff,
			 struct filerec *file, uint64_t unshared);
int dedupe_extents(struct dedupe_ctxt *ctxt);
/*
 * `bytes_freed` is the destination's `unshared` bytes, capped at what the
 * kernel actually processed - i.e. space this dedupe stopped duplicating, not
 * length it compared. `bytes_done` is what the kernel processed. Both count
 * the rounds that succeeded even when a later one failed (#280): the 32 MiB
 * rounds are separate ioctls, and what an earlier one shared stays shared.
 */
int pop_one_dedupe_result(struct dedupe_ctxt *ctxt, int *status,
			  uint64_t *bytes_freed, uint64_t *bytes_done,
			  struct filerec **file);

/* What a FIDEDUPERANGE probe learned about a filesystem (#224). */
enum dedupe_support {
	DEDUPE_SUPPORT_NO,	/* the filesystem does not implement the ioctl */
	DEDUPE_SUPPORT_YES,	/* it does */
	DEDUPE_SUPPORT_UNKNOWN,	/* this file could not answer the question */
};

/*
 * Classify one probe result from the ioctl's return and the per-destination
 * status it fills in. Pure, so the taxonomy can be unit-tested against every
 * errno that matters instead of against whichever filesystem the test host
 * happens to have.
 *
 * Reading `status` is not optional; see dedupe_probe_fd for why, and for the
 * measurements behind it.
 */
enum dedupe_support dedupe_classify_probe(int rc, int err, int64_t status);

/*
 * The length of each of the two ranges the probe compares in the file behind
 * `fd`, `size` bytes long, or 0 when it is too small to hold them. Such a file
 * is not asked at all.
 */
uint64_t dedupe_probe_len(int fd, uint64_t size);

/*
 * Ask the filesystem behind `fd` whether it implements FIDEDUPERANGE. `fd`
 * must be one the ioctl accepts as a destination (open for writing, or a file
 * the caller owns) and refer to a regular file of `size` bytes; a file too
 * small to hold two disjoint ranges cannot answer.
 */
enum dedupe_support dedupe_probe_fd(int fd, uint64_t size);

#endif	/* __DEDUPE_H__ */
