/*
 * opt.h
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

#ifndef	__OPT_H__
#define	__OPT_H__

#include <stdbool.h>
#include <stdint.h>

struct options {
	int run_dedupe;
	bool recurse_dirs : 1;
	/*
	 * Worker-thread counts. cpu_threads is defaulted before option parsing;
	 * io_threads stays 0 (== auto) until apply_storage_defaults() resolves it
	 * from the scan target's storage once the roots are known. A non-zero
	 * value here means the user set it explicitly and auto-tuning is skipped.
	 */
	unsigned int io_threads;
	unsigned int cpu_threads;
	bool skip_zeroes : 1;
	/*
	 * Keep read-only btrfs subvolumes (snapshots) out of the scan. Off by
	 * default: the kernel deduplicates *into* a read-only subvolume quite
	 * happily, so snapshots are ordinary dedupe material and a backup target
	 * made of them is a workload oans exists to serve (#182). Worth turning
	 * on where snapshots are near-copies of the live subvolume already
	 * sharing its extents - a snapper/Timeshift desktop - since there the
	 * read and hash of every file in every snapshot buys nothing and the
	 * cost scales with snapshot count.
	 */
	bool skip_readonly_subvols : 1;
	bool only_whole_files : 1;
	bool do_block_hash : 1;
	bool dedupe_same_file : 1;
	unsigned int batch_size;
	char *hashfile;
	uint64_t min_filesize;	/* skip regular files smaller than this */
	uint64_t max_filesize;	/* skip regular files larger than this; 0 = no limit */
	bool progress_json : 1;	/* stream JSONL progress to stderr, no ANSI UI */
};

extern struct options options;

#endif	/* __OPT_H__ */
