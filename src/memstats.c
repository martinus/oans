/*
 * memstats.c
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
 * Authors: Mark Fasheh <mfasheh@suse.de>
 */


#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>

#include "memstats.h"
#include "debug.h"
#include "file_scan.h"
#include "filerec.h"
#include "util.h"

void print_mem_stats(void)
{
	uint64_t sqlite3_highwater, sqlite3_memused;

	printf("oans memory usage statistics:\n");
	show_allocs_file_block();
	show_allocs_dupe_blocks_list();
	show_allocs_dupe_extents();
	show_allocs_extent();
	show_allocs_filerec();
	show_allocs_filerec_token();
	show_allocs_file_hash_head();
	sqlite3_highwater = sqlite3_memory_highwater(0);
	sqlite3_memused = sqlite3_memory_used();
	printf("Sqlite3 used: %"PRIu64"  highwater: %"PRIu64"\n",
	       sqlite3_memused, sqlite3_highwater);
}

/* Resident set size, in bytes, from /proc/self/statm (field 2, in pages). */
static uint64_t rss_bytes(void)
{
	unsigned long long total, resident = 0;
	FILE *f = fopen("/proc/self/statm", "re");

	if (!f)
		return 0;
	if (fscanf(f, "%llu %llu", &total, &resident) != 2)
		resident = 0;
	fclose(f);
	return (uint64_t)resident * (uint64_t)sysconf(_SC_PAGESIZE);
}

/*
 * One line per source of memory, at the moment `when` names (#208).
 *
 * Attribution, not a total: RSS is what the kernel charges the process, and the
 * lines below are what oans can account for. The gap between them is glibc's
 * arenas holding freed chunks, thread stacks, and the binary itself - which is
 * exactly what has to be measured before deciding whether any of the per-file
 * structures is worth shrinking.
 *
 * sqlite is asked for its high-water mark as well as its current use, because
 * the page caches fill toward their per-connection cap and then stay: the peak
 * is the figure a memory budget has to be built from.
 */
void print_mem_breakdown(const char *when)
{
	struct scan_mem_stats scan;
	struct mallinfo2 mi;
	uint64_t rss, sql, sql_peak;

	/* Gated here rather than at the call sites, like report_scan_stats(). */
	if (!getenv("DUPEREMOVE_MEM_STATS"))
		return;

	mi = mallinfo2();
	rss = rss_bytes();
	sql = (uint64_t)sqlite3_memory_used();
	sql_peak = (uint64_t)sqlite3_memory_highwater(0);
	filescan_get_mem_stats(&scan);

	eprintf("mem-stats [%s]\n", when);
	eprintf("  rss                 %10s\n", human_size(rss));
	eprintf("  malloc in use       %10s (arena %s, mmap %s)\n",
		human_size(mi.uordblks), human_size(mi.arena),
		human_size(mi.hblkhd));
	eprintf("  sqlite now/peak     %10s / %s\n",
		human_size(sql), human_size(sql_peak));
	eprintf("  walk queue peak    ~%10s (%"PRIu64" items)\n",
		human_size(scan.walk_peak_bytes), scan.walk_queued);
	eprintf("  csum queue peak    ~%10s (%"PRIu64" items)\n",
		human_size(scan.csum_peak_bytes), scan.csum_queued);
	eprintf("  seen_inodes         %10s (%"PRIu64" entries)\n",
		human_size(scan.seen_inodes_bytes), scan.seen_inodes);
	eprintf("  seen_files bitmap   %10s\n",
		human_size(scan.seen_files_bytes));
	eprintf("  filerecs            %10s (%llu live)\n",
		human_size(num_filerecs * sizeof(struct filerec)), num_filerecs);
}
