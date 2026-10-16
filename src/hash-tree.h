/*
 * hash-tree.h
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

#ifndef __HASH_TREE__
#define __HASH_TREE__
#include "debug.h"

#include "file_flags.h"

extern unsigned int blocksize;

struct hash_tree {
	struct rb_root	root;
	uint64_t	num_blocks;
	uint64_t	num_hashes;
};

struct dupe_blocks_list {
	struct rb_node		dl_node; /* sorted by hash */

	unsigned long long	dl_num_elem;
	struct list_head	dl_list;

	unsigned char		dl_hash[DIGEST_LEN];
};

struct file_block {
	struct dupe_blocks_list	*b_parent;
	struct filerec	*b_file;
	uint64_t	b_loff;

	/*
	 * All blocks with this hash. Can be on dupe_blocks_list->dl_list, or
	 * block_dedupe_list->bd_block_list (see run_dedupe.c).
	 */
	struct list_head	b_list;

	struct rb_node		b_file_next; /* filerec->block_tree, by b_loff */
};

struct dupe_blocks_list *find_block_list(struct hash_tree *tree,
					 unsigned char *digest);

int insert_hashed_block(struct hash_tree *tree, unsigned char *digest,
			struct filerec *file, uint64_t loff);
int remove_hashed_block(struct hash_tree *tree, struct file_block *block);
struct file_block *find_filerec_block(struct filerec *file,
				      uint64_t loff);

void init_hash_tree(struct hash_tree *tree);
void free_hash_tree(struct hash_tree *tree);
#endif /* __HASH_TREE__ */
