/*
 * csum.h
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

#ifndef __CSUM_H__
#define __CSUM_H__

#include <stdio.h>
#include <stdint.h>

#define	DIGEST_LEN	16
#define	HASH_TYPE	"XXHASH3 "

/* Debug */
void debug_print_digest_len(FILE *stream, unsigned char *digest, unsigned int len);
static inline void debug_print_digest(FILE *stream, unsigned char *digest)
{
	debug_print_digest_len(stream, digest, DIGEST_LEN);
}
static inline void debug_print_digest_short(FILE *stream, unsigned char *digest)
{
	debug_print_digest_len(stream, digest, 4);
}

/* Checksums a single block in one go. */
void checksum_block(char *buf, int len, unsigned char *digest);

/*
 * Hash a NUL-terminated path to a 64-bit value. Used as a compact stand-in for
 * the full path in the files table's uniqueness index (the path text itself is
 * still stored, since files must be opened by name to dedupe).
 */
uint64_t csum_path(const char *path);

/* Keeping a 'running' checksum - we add data to it a bit at a time */
struct running_checksum;
struct running_checksum *start_running_checksum(void);
void add_to_running_checksum(struct running_checksum *c,
			     unsigned char *buf, unsigned int len);
void finish_running_checksum(struct running_checksum *c, unsigned char *digest);

#define	DECLARE_RUNNING_CSUM_CAST_FUNCS(_type)				\
static inline struct _type *						\
rc_to_priv(struct running_checksum *rc)					\
{									\
	return (struct _type *)rc;					\
}									\
static inline struct running_checksum *					\
priv_to_rc(struct _type *priv)						\
{									\
	return (struct running_checksum *)priv;				\
}

#endif	/* csum.h */
