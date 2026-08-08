/*
 * csum.c
 *
 * Copyright (C) 2014 SUSE.  All rights reserved.
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

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csum.h"
#include "debug.h"
#include "util.h"

#define XXH_STATIC_LINKING_ONLY
#define XXH_INLINE_ALL
#include "xxhash.h"

struct xxhash_running_checksum {
        XXH3_state_t *state;
};
DECLARE_RUNNING_CSUM_CAST_FUNCS(xxhash_running_checksum);

void debug_print_digest_len(FILE *stream, unsigned char *digest, unsigned int len)
{
	uint32_t i;

	abort_on(len > DIGEST_LEN);

	for (i = 0; i < len; i++)
		fprintf(stream, "%.2x", digest[i]);
}

/* Serialize a 128-bit xxhash into digest. The one place the layout is defined. */
static inline void store_digest(unsigned char *digest, XXH128_hash_t hash)
{
	((uint64_t*)digest)[0] = hash.low64;
	((uint64_t*)digest)[1] = hash.high64;
}

void checksum_block(char *buf, int len, unsigned char *digest)
{
	store_digest(digest, XXH128(buf, len, 0));
}

uint64_t csum_path(const char *path)
{
	return XXH3_64bits(path, strlen(path));
}

struct running_checksum *start_running_checksum(void)
{
	struct xxhash_running_checksum *c =
		calloc(1, sizeof(struct xxhash_running_checksum));
	c->state = XXH3_createState();
	/*
	 * Zero the whole struct before resetting it, which xxhash sanctions
	 * (XXH3_INITSTATE is a field-wise version of the same thing). A reset
	 * only initialises the fields it uses, so the alignment padding and the
	 * unused tail of the internal buffer stay whatever the allocator left -
	 * fine while the state never leaves the process, but running_checksum_
	 * save() copies it out verbatim and those bytes would then be written to
	 * the hashfile uninitialised. Caught by valgrind, and worth keeping
	 * caught: a snapshot must be fully defined bytes.
	 */
	memset(c->state, 0, sizeof(XXH3_state_t));
	XXH3_128bits_reset(c->state);
	return priv_to_rc(c);
}

void add_to_running_checksum(struct running_checksum *_c,
			     unsigned char *buf, unsigned int len)
{
	struct xxhash_running_checksum *c = rc_to_priv(_c);
	XXH3_128bits_update(c->state, buf, len);
}

void finish_running_checksum(struct running_checksum *_c, unsigned char *digest)
{
	_cleanup_(freep) struct xxhash_running_checksum *c = rc_to_priv(_c);

	XXH128_hash_t hash = XXH3_128bits_digest(c->state);

	if (digest)
		store_digest(digest, hash);
	XXH3_freeState(c->state);
}

/*
 * Serialized form of an in-progress XXH3 hash: a header identifying the exact
 * layout, then the state struct verbatim.
 *
 * xxhash publishes no serialization of XXH3_state_t, so this snapshots the
 * struct itself - which is legitimate (XXH3_copyState is a plain copy of those
 * same bytes) but only within one build. The header pins everything the layout
 * depends on: the library version, the size of the struct, and the byte order.
 * Anything unrecognised is refused rather than reinterpreted, so the cost of
 * being wrong is a rehash, never a wrong digest. oans links xxhash from the
 * system, so a distro upgrade genuinely can invalidate a saved state.
 */
#define CSUM_STATE_MAGIC	0x4f414e5343534d31ULL	/* "OANSCSM1" */

struct csum_state_hdr {
	uint64_t	magic;
	uint32_t	xxh_version;	/* XXH_versionNumber() at save time */
	uint32_t	state_size;	/* sizeof(XXH3_state_t) at save time */
};

size_t running_checksum_state_size(void)
{
	return sizeof(struct csum_state_hdr) + sizeof(XXH3_state_t);
}

int running_checksum_save(struct running_checksum *_c, void *buf, size_t len)
{
	struct xxhash_running_checksum *c = rc_to_priv(_c);
	struct csum_state_hdr hdr = {
		.magic		= CSUM_STATE_MAGIC,
		.xxh_version	= XXH_versionNumber(),
		.state_size	= sizeof(XXH3_state_t),
	};

	if (len < running_checksum_state_size())
		return -EINVAL;

	memcpy(buf, &hdr, sizeof(hdr));
	memcpy((char *)buf + sizeof(hdr), c->state, sizeof(XXH3_state_t));
	return 0;
}

struct running_checksum *running_checksum_restore(const void *buf, size_t len)
{
	struct csum_state_hdr hdr;
	struct xxhash_running_checksum *c;
	const unsigned char *secret;

	if (len != running_checksum_state_size())
		return NULL;

	memcpy(&hdr, buf, sizeof(hdr));
	if (hdr.magic != CSUM_STATE_MAGIC ||
	    hdr.xxh_version != XXH_versionNumber() ||
	    hdr.state_size != sizeof(XXH3_state_t))
		return NULL;

	c = rc_to_priv(start_running_checksum());
	if (!c)
		return NULL;

	/*
	 * The state holds a pointer to the library's static default secret, so
	 * the saved copy of it means nothing in this process. Everything else
	 * is plain data: take the freshly-reset state's pointer and overwrite
	 * the rest.
	 */
	secret = c->state->extSecret;
	memcpy(c->state, (const char *)buf + sizeof(hdr), sizeof(XXH3_state_t));
	c->state->extSecret = secret;

	return priv_to_rc(c);
}
