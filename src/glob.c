/*
 * glob.c
 *
 * gitignore-style path matching for --exclude. See glob.h for the syntax.
 *
 * Patterns are translated to PCRE2 fragments and joined into a single combined
 * regex, so matching costs one regex run per path regardless of how many
 * patterns there are. Exact paths (the hashfile and its sidecars, which oans
 * excludes on the user's behalf) skip the regex entirely via a hash lookup.
 *
 * The per-pattern regexes are kept as well, but only to work out *which*
 * pattern caused a hit - for the -v message, and to apply the directory-only
 * rule. That scan runs only when the combined regex already matched, i.e. on a
 * path we are about to skip anyway, never on the hot negative path.
 *
 * GRegex is PCRE2 underneath and GLib is already linked, so this adds no
 * dependency.
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

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include <glib.h>

#include "glob.h"

struct glob_pat {
	char		*pattern;	/* as written; also the literal hash key */
	GRegex		*re;		/* NULL for exact-match entries, or
					 * a pattern that is not UTF-8 */
	GRegex		*re_raw;	/* the same over bytes; see raw_path() */
	bool		dir_only;
	bool		internal;	/* added by oans, not by the user */
	/*
	 * Set from the walker threads, which run concurrently. Only ever
	 * flipped false->true, and only its final value is read (after the
	 * walk, to report patterns that matched nothing), so a relaxed atomic
	 * is enough - and writing it only on the first hit keeps the shared
	 * cache line off the hot path.
	 */
	_Atomic bool	matched;
};

struct glob_set {
	GPtrArray	*pats;		/* struct glob_pat *, in add order */
	GHashTable	*literals;	/* pattern -> struct glob_pat * */
	GRegex		*re;		/* every glob fragment, one alternation */
	GRegex		*re_raw;	/* the same, compiled over bytes */
};

static void glob_pat_free(gpointer p)
{
	struct glob_pat *gp = p;

	g_clear_pointer(&gp->re, g_regex_unref);
	g_clear_pointer(&gp->re_raw, g_regex_unref);
	g_free(gp->pattern);
	g_free(gp);
}

struct glob_set *glob_set_new(void)
{
	struct glob_set *gs = g_malloc0(sizeof(*gs));

	gs->pats = g_ptr_array_new_with_free_func(glob_pat_free);
	gs->literals = g_hash_table_new(g_str_hash, g_str_equal);
	return gs;
}

void glob_set_free(struct glob_set *gs)
{
	if (!gs)
		return;
	g_clear_pointer(&gs->re, g_regex_unref);
	g_clear_pointer(&gs->re_raw, g_regex_unref);
	g_hash_table_destroy(gs->literals);
	g_ptr_array_free(gs->pats, TRUE);
	g_free(gs);
}

/*
 * Translate a glob character class starting at pat[*i] == '['. On success
 * advances *i to the closing ']' (the caller's loop steps past it) and returns
 * true; on an unterminated class rolls `out` back and returns false.
 */
static bool append_class(GString *out, const char *pat, size_t len, size_t *i)
{
	size_t mark = out->len;
	size_t j = *i + 1;

	bool negated = j < len && (pat[j] == '!' || pat[j] == '^');

	/* A class never matches the separator (#283), negated or not: gitignore
	 * matches a name, and '/' is never part of one. */
	g_string_append(out, negated ? "[^/" : "(?!/)[");
	if (negated)
		j++;
	/* A ']' immediately after the (possibly negated) open bracket is data. */
	if (j < len && pat[j] == ']') {
		g_string_append(out, "\\]");
		j++;
	}

	for (; j < len; j++) {
		if (pat[j] == ']') {
			g_string_append_c(out, ']');
			*i = j;
			return true;
		}
		/* A POSIX class, [:digit:] and the like, is PCRE2 syntax too. */
		if (pat[j] == '[' && j + 1 < len && pat[j + 1] == ':') {
			size_t k = j + 2;

			while (k < len && g_ascii_isalpha(pat[k]))
				k++;
			if (k > j + 2 && k + 1 < len && pat[k] == ':' &&
			    pat[k + 1] == ']') {
				g_string_append_len(out, pat + j, k + 2 - j);
				j = k + 1;
				continue;
			}
		}
		/* '-' and ranges pass through; only these two would change
		 * meaning inside a PCRE2 class. */
		if (pat[j] == '\\' || pat[j] == '[')
			g_string_append_c(out, '\\');
		g_string_append_c(out, pat[j]);
	}

	g_string_truncate(out, mark);
	return false;
}

/*
 * Compile one gitignore-style pattern into an anchored PCRE2 fragment.
 * Returns a newly allocated string, or NULL with *err set.
 */
static char *glob_to_regex(const char *pat, bool *dir_only, char **err)
{
	size_t len = strlen(pat);
	GString *re;

	*dir_only = false;
	while (len > 0 && pat[len - 1] == '/') {
		*dir_only = true;
		len--;
	}
	if (len == 0) {
		*err = g_strdup_printf("empty exclude pattern \"%s\"", pat);
		return NULL;
	}

	re = g_string_new(NULL);
	if (pat[0] == '/')
		g_string_append_c(re, '^');		/* absolute */
	else if (memchr(pat, '/', len))
		g_string_append(re, "^(?:.*/)?");	/* any depth */
	else
		g_string_append(re, "(?:^|/)");		/* basename */

	for (size_t i = 0; i < len; i++) {
		char c = pat[i];

		if (c == '*') {
			size_t stars = 0;

			while (i + stars < len && pat[i + stars] == '*')
				stars++;
			if (stars == 1) {
				g_string_append(re, "[^/]*");
			} else if (i + stars < len && pat[i + stars] == '/') {
				/* '**' then a separator: zero or more dirs */
				g_string_append(re, "(?:.*/)?");
				i += stars;
			} else {
				g_string_append(re, ".*");
				i += stars - 1;
			}
		} else if (c == '?') {
			g_string_append(re, "[^/]");
		} else if (c == '[') {
			if (!append_class(re, pat, len, &i)) {
				*err = g_strdup_printf(
					"unterminated '[' in exclude pattern \"%s\"",
					pat);
				g_string_free(re, TRUE);
				return NULL;
			}
		} else {
			/* Escape via GLib rather than a hand-kept metacharacter
			 * list, which could drift from PCRE2's. */
			char one[2] = { (c == '\\' && i + 1 < len) ? pat[++i] : c, 0 };
			char *esc = g_regex_escape_string(one, 1);

			g_string_append(re, esc);
			g_free(esc);
		}
	}
	g_string_append_c(re, '$');

	return g_string_free(re, FALSE);
}

/*
 * An absolute path with no metacharacter needs no regex at all, so it can go
 * in the literal set: a hash lookup, and no chance of a stray '*' in someone's
 * hashfile path behaving as a wildcard.
 */
static bool is_plain_path(const char *pat)
{
	return pat[0] == '/' && !strpbrk(pat, "*?[\\") &&
	       !g_str_has_suffix(pat, "/");
}

static struct glob_pat *pat_new(struct glob_set *gs, const char *pattern)
{
	struct glob_pat *gp = g_malloc0(sizeof(*gp));

	gp->pattern = g_strdup(pattern);
	g_ptr_array_add(gs->pats, gp);
	return gp;
}

static void add_literal(struct glob_set *gs, const char *path, bool internal)
{
	struct glob_pat *gp = pat_new(gs, path);

	gp->internal = internal;
	g_hash_table_insert(gs->literals, gp->pattern, gp);
}

void glob_set_add_literal(struct glob_set *gs, const char *path)
{
	add_literal(gs, path, true);
}

/*
 * G_REGEX_OPTIMIZE turns on PCRE2's JIT. It is not optional here: matching runs
 * once per directory entry on every walker thread, and measured ~12x slower
 * without it.
 *
 * DOTALL and DOLLAR_ENDONLY because a name may contain a newline (#283):
 * without them `**` and the any-depth prefix stop at one, which let a crafted
 * name out from under an exclude, and `$` also matched before a trailing one,
 * so `foo` excluded `foo\n`.
 */
#define GLOB_REGEX_FLAGS \
	(G_REGEX_OPTIMIZE | G_REGEX_DOTALL | G_REGEX_DOLLAR_ENDONLY)

/*
 * Whether `path` has to be matched over bytes. GRegex compiles for UTF-8, where
 * `?` and a class take one character, and PCRE2 leaves matching a name that
 * is not valid UTF-8 undefined - in practice no wildcard crosses a stray byte,
 * so `*.iso` did not exclude a Latin-1 `caf\xe9.iso` (#283). Such a name is
 * matched with the same patterns compiled G_REGEX_RAW, where a byte is a
 * character - which is what it is in the Latin-1 names that produce them.
 */
static bool raw_path(const char *path)
{
	return !g_utf8_validate(path, -1, NULL);
}

int glob_set_add(struct glob_set *gs, const char *pattern, char **err)
{
	struct glob_pat *gp;
	bool dir_only = false;
	char *frag;
	GError *gerr = NULL;

	if (is_plain_path(pattern)) {
		add_literal(gs, pattern, false);
		return 0;
	}

	frag = glob_to_regex(pattern, &dir_only, err);
	if (!frag)
		return 1;

	gp = pat_new(gs, pattern);
	gp->dir_only = dir_only;
	/* A pattern that is not UTF-8 has only the byte form. */
	if (g_utf8_validate(frag, -1, NULL))
		gp->re = g_regex_new(frag, GLOB_REGEX_FLAGS, 0, &gerr);
	if (!gerr)
		gp->re_raw = g_regex_new(frag, GLOB_REGEX_FLAGS | G_REGEX_RAW,
					 0, &gerr);
	g_free(frag);
	if (gerr) {
		*err = g_strdup_printf("bad exclude pattern \"%s\": %s",
				       pattern, gerr->message);
		g_error_free(gerr);
		return 1;
	}
	return 0;
}

/* Join every pattern's regex of one form into one alternation. */
static int compile_one(struct glob_set *gs, bool raw, GRegex **out, char **err)
{
	GString *all = g_string_new(NULL);
	GError *gerr = NULL;
	unsigned int n = 0;
	int ret = 0;

	for (unsigned int i = 0; i < gs->pats->len; i++) {
		struct glob_pat *gp = g_ptr_array_index(gs->pats, i);
		GRegex *re = raw ? gp->re_raw : gp->re;

		if (!re)
			continue;
		if (n++)
			g_string_append_c(all, '|');
		g_string_append_printf(all, "(?:%s)", g_regex_get_pattern(re));
	}

	if (n) {
		*out = g_regex_new(all->str,
				   GLOB_REGEX_FLAGS | (raw ? G_REGEX_RAW : 0),
				   0, &gerr);
		if (!*out) {
			*err = g_strdup_printf("combining exclude patterns: %s",
					       gerr->message);
			g_error_free(gerr);
			ret = 1;
		}
	}
	g_string_free(all, TRUE);
	return ret;
}

int glob_set_compile(struct glob_set *gs, char **err)
{
	g_clear_pointer(&gs->re, g_regex_unref);
	g_clear_pointer(&gs->re_raw, g_regex_unref);

	return compile_one(gs, false, &gs->re, err) ||
	       compile_one(gs, true, &gs->re_raw, err);
}

/*
 * Which pattern matched? Only reached once the combined regex has already said
 * yes, so this linear scan runs at most once per excluded path - and it is
 * also where the directory-only rule is applied, since the combined regex does
 * not distinguish.
 */
static struct glob_pat *attribute(struct glob_set *gs, const char *path,
				  bool is_dir, bool raw)
{
	for (unsigned int i = 0; i < gs->pats->len; i++) {
		struct glob_pat *gp = g_ptr_array_index(gs->pats, i);
		GRegex *re = raw ? gp->re_raw : gp->re;

		if (!re || (gp->dir_only && !is_dir))
			continue;
		if (g_regex_match(re, path, 0, NULL))
			return gp;
	}
	return NULL;
}

bool glob_set_match(struct glob_set *gs, const char *path, bool is_dir,
		    const char **which)
{
	struct glob_pat *gp = g_hash_table_lookup(gs->literals, path);

	if (!gp) {
		bool raw = raw_path(path);
		GRegex *re = raw ? gs->re_raw : gs->re;

		if (!re || !g_regex_match(re, path, 0, NULL))
			return false;
		/* A directory-only pattern can match the combined regex on a
		 * plain file; attribute() is what rejects it. */
		gp = attribute(gs, path, is_dir, raw);
		if (!gp)
			return false;
	}

	if (!atomic_load_explicit(&gp->matched, memory_order_relaxed))
		atomic_store_explicit(&gp->matched, true, memory_order_relaxed);
	if (which)
		*which = gp->pattern;
	return true;
}

bool glob_set_stat(const struct glob_set *gs, unsigned int i,
		   const char **pattern, bool *matched)
{
	unsigned int seen = 0;

	for (unsigned int j = 0; j < gs->pats->len; j++) {
		struct glob_pat *gp = g_ptr_array_index(gs->pats, j);

		if (gp->internal)
			continue;	/* the hashfile and its sidecars */
		if (seen++ != i)
			continue;
		*pattern = gp->pattern;
		*matched = atomic_load_explicit(&gp->matched,
						memory_order_relaxed);
		return true;
	}
	return false;
}
