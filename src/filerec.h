/*
 * filerec.h
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
 */

#ifndef __FILEREC__
#define __FILEREC__

#include <stdint.h>
#include <time.h>
#include <glib.h>
#include <bsd/sys/queue.h>
#include "rbtree.h"
#include "results-tree.h"

SLIST_HEAD(filerec_list, filerec);
extern struct filerec_list filerec_head;

extern unsigned long long num_filerecs;
extern unsigned int dedupe_seq; /* This is incremented on every dedupe pass */

struct filerec {
	int		fd;			/* file descriptor */
	unsigned int	fd_refs;			/* fd refcount */
	/*
	 * Lifetime refcount, distinct from fd_refs. In the streaming dedupe
	 * phase a filerec can be referenced by more than one in-flight batch
	 * (e.g. a cross-window anchor); each batch that loads it holds one ref
	 * and drops it at batch completion. All get/put happen on the single
	 * producer thread, so this needs no lock. filerec_new() starts at 0.
	 */
	unsigned int	refs;

	char	*filename;		/* path to file */
	int64_t fileid;

	struct rb_node		fileid_node;

	uint64_t		size;
	struct rb_root		block_tree;	/* root for hash blocks tree */

	SLIST_ENTRY(filerec)	rec_list;	/* all filerecs */
};

void init_filerec(void);
void free_all_filerecs(void);

struct filerec *filerec_new(const char *filename, int64_t fileid,
			    uint64_t size);
struct filerec *filerec_find(int64_t fileid);

/*
 * Lifetime refcount (see struct filerec::refs). filerec_put() frees the filerec
 * when its count reaches zero. Producer-thread only; not thread-safe by design.
 */
void filerec_get(struct filerec *file);
void filerec_put(struct filerec *file);

int filerec_open(struct filerec *file, bool quiet);
void filerec_close(struct filerec *file);

struct open_once {
	struct rb_root	root;
};
#define	OPEN_ONCE_INIT	(struct open_once) { RB_ROOT, }
#define OPEN_ONCE(name)	struct open_once name = OPEN_ONCE_INIT

int filerec_open_once(struct filerec *file,
		      struct open_once *open_files);
void filerec_close_open_list(struct open_once *open_files);

/*
 * Track unique filerecs in a tree. Two places in the code use this:
 *	- filerec comparison tracking in filerec.c
 *	- conversion of large dupe lists in hash-tree.c
 * User has to define an rb_root, and a "free all" function.
 */
struct filerec_token {
	struct filerec	*t_file;
	struct rb_node	t_node;
};
struct filerec_token *find_filerec_token_rb(struct rb_root *root,
					    struct filerec *val);
void insert_filerec_token_rb(struct rb_root *root,
			     struct filerec_token *token);
void filerec_token_free(struct filerec_token *token);
struct filerec_token *filerec_token_new(struct filerec *file);

int fiemap_scan_extent(struct extent *extent);
#endif /* __FILEREC__ */
