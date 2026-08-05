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
	 * Skip read-only btrfs subvolumes (snapshots) when deduplicating. A
	 * read-only subvolume can never be a dedupe *destination* -- the kernel
	 * refuses -- so under -d the read and hash of every file in it is
	 * provably wasted, not merely usually wasted. Tri-state so an explicit
	 * --[no-]skip-readonly-subvols beats the -d-derived default (#156).
	 */
	int skip_readonly_subvols;	/* -1 = auto (on iff -d), 0 = no, 1 = yes */
	bool only_whole_files : 1;
	bool do_block_hash : 1;
	bool dedupe_same_file : 1;
	unsigned int batch_size;
	char *hashfile;
	uint64_t min_filesize;	/* skip regular files smaller than this */
	bool progress_json : 1;	/* stream JSONL progress to stderr, no ANSI UI */
};

extern struct options options;

#endif	/* __OPT_H__ */
