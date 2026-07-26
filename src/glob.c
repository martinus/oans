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
 * The per-pattern regexes are kept as well, but only to attribute a hit to the
 * pattern that caused it for the -v message. That runs only when the combined
 * regex already matched, i.e. on a path we are about to skip anyway - never on
 * the hot negative path.
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

#include <stdbool.h>
#include <string.h>

#include <glib.h>

#include "glob.h"

struct glob_pat {
	char		*pattern;	/* as written, for diagnostics */
	char		*literal;	/* exact-match entries only */
	GRegex		*re;		/* glob entries only */
	bool		dir_only;
	bool		internal;	/* added by oans, not by the user */
	unsigned long	matches;
};

struct glob_set {
	GPtrArray	*pats;		/* struct glob_pat *, in add order */
	GHashTable	*literals;	/* literal -> struct glob_pat * */
	GRegex		*any;		/* combined, patterns matching any path */
	GRegex		*dir;		/* combined, directory-only patterns */
	bool		compiled;
};

static void glob_pat_free(gpointer p)
{
	struct glob_pat *gp = p;

	if (gp->re)
		g_regex_unref(gp->re);
	g_free(gp->pattern);
	g_free(gp->literal);
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
	if (gs->any)
		g_regex_unref(gs->any);
	if (gs->dir)
		g_regex_unref(gs->dir);
	g_hash_table_destroy(gs->literals);
	g_ptr_array_free(gs->pats, TRUE);
	g_free(gs);
}

bool glob_set_empty(const struct glob_set *gs)
{
	return gs->pats->len == 0;
}

/* Characters PCRE2 treats specially, which a literal glob byte must escape. */
static void append_literal_char(GString *out, char c)
{
	if (strchr("\\^$.[]|()*+?{}", c))
		g_string_append_c(out, '\\');
	g_string_append_c(out, c);
}

/*
 * Translate a glob character class starting at pat[*i] == '['. On success
 * advances *i to the closing ']' (the caller's loop steps past it) and returns
 * true; returns false if the class is unterminated.
 */
static bool append_class(GString *out, const char *pat, size_t len, size_t *i)
{
	GString *cls = g_string_new("[");
	size_t j = *i + 1;
	bool closed = false;

	/* Both spellings of negation; PCRE2 only knows '^'. */
	if (j < len && (pat[j] == '!' || pat[j] == '^')) {
		g_string_append_c(cls, '^');
		j++;
	}
	/* A ']' immediately after the (possibly negated) open bracket is data. */
	if (j < len && pat[j] == ']') {
		g_string_append(cls, "\\]");
		j++;
	}

	for (; j < len; j++) {
		if (pat[j] == ']') {
			closed = true;
			break;
		}
		/* '-' and ranges pass through; only these two would change
		 * meaning inside a PCRE2 class. */
		if (pat[j] == '\\' || pat[j] == '[')
			g_string_append_c(cls, '\\');
		g_string_append_c(cls, pat[j]);
	}

	if (!closed) {
		g_string_free(cls, TRUE);
		return false;
	}

	g_string_append_c(cls, ']');
	g_string_append(out, cls->str);
	g_string_free(cls, TRUE);
	*i = j;
	return true;
}

/*
 * Compile one gitignore-style pattern into an anchored PCRE2 fragment.
 * Returns a newly allocated string, or NULL with *err set.
 */
static char *glob_to_regex(const char *pat, bool *dir_only, char **err)
{
	size_t len = strlen(pat);
	bool has_slash = false;
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

	for (size_t i = 0; i < len; i++) {
		if (pat[i] == '/') {
			has_slash = true;
			break;
		}
	}

	re = g_string_new(NULL);
	if (pat[0] == '/')
		g_string_append_c(re, '^');		/* absolute */
	else if (has_slash)
		g_string_append(re, "^(?:.*/)?");	/* any depth */
	else
		g_string_append(re, "(?:^|/)");		/* basename */

	for (size_t i = 0; i < len; i++) {
		char c = pat[i];

		if (c == '*') {
			if (i + 1 < len && pat[i + 1] == '*') {
				i++;
				if (i + 1 < len && pat[i + 1] == '/') {
					/* '**' then '/': zero or more dirs */
					g_string_append(re, "(?:.*/)?");
					i++;
				} else {
					g_string_append(re, ".*");
				}
			} else {
				g_string_append(re, "[^/]*");
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
		} else if (c == '\\' && i + 1 < len) {
			append_literal_char(re, pat[++i]);
		} else {
			append_literal_char(re, c);
		}
	}
	g_string_append_c(re, '$');

	return g_string_free(re, FALSE);
}

/* True if the pattern has no metacharacter, so it can go in the literal set. */
static bool is_plain_path(const char *pat)
{
	return pat[0] == '/' && !strpbrk(pat, "*?[\\");
}

static struct glob_pat *pat_new(struct glob_set *gs, const char *pattern)
{
	struct glob_pat *gp = g_malloc0(sizeof(*gp));

	gp->pattern = g_strdup(pattern);
	g_ptr_array_add(gs->pats, gp);
	return gp;
}

static int add_literal(struct glob_set *gs, const char *path, bool internal)
{
	struct glob_pat *gp = pat_new(gs, path);

	gp->literal = g_strdup(path);
	gp->internal = internal;
	g_hash_table_insert(gs->literals, gp->literal, gp);
	gs->compiled = false;
	return 0;
}

int glob_set_add_literal(struct glob_set *gs, const char *path)
{
	return add_literal(gs, path, true);
}

int glob_set_add(struct glob_set *gs, const char *pattern, char **err)
{
	struct glob_pat *gp;
	bool dir_only = false;
	char *frag;
	GError *gerr = NULL;

	*err = NULL;

	/*
	 * An absolute path with no metacharacters is the common case for the
	 * excludes oans adds itself and for a user naming one directory; a hash
	 * lookup beats the regex.
	 */
	if (is_plain_path(pattern) && pattern[strlen(pattern) - 1] != '/')
		return add_literal(gs, pattern, false);

	frag = glob_to_regex(pattern, &dir_only, err);
	if (!frag)
		return 1;

	gp = pat_new(gs, pattern);
	gp->dir_only = dir_only;
	gp->re = g_regex_new(frag, 0, 0, &gerr);
	g_free(frag);
	if (!gp->re) {
		*err = g_strdup_printf("bad exclude pattern \"%s\": %s",
				       pattern, gerr->message);
		g_error_free(gerr);
		return 1;
	}
	gs->compiled = false;
	return 0;
}

/* Join every fragment of one flavour into a single alternation. */
static GRegex *compile_combined(struct glob_set *gs, bool dir_only, char **err)
{
	GString *all = g_string_new(NULL);
	GRegex *re = NULL;
	GError *gerr = NULL;
	unsigned int n = 0;

	for (unsigned int i = 0; i < gs->pats->len; i++) {
		struct glob_pat *gp = g_ptr_array_index(gs->pats, i);
		const gchar *frag;

		if (!gp->re || gp->dir_only != dir_only)
			continue;
		frag = g_regex_get_pattern(gp->re);
		if (n++)
			g_string_append_c(all, '|');
		g_string_append_printf(all, "(?:%s)", frag);
	}

	if (n) {
		re = g_regex_new(all->str, 0, 0, &gerr);
		if (!re) {
			*err = g_strdup_printf("combining exclude patterns: %s",
					       gerr->message);
			g_error_free(gerr);
		}
	}
	g_string_free(all, TRUE);
	return re;
}

int glob_set_compile(struct glob_set *gs, char **err)
{
	*err = NULL;

	if (gs->any) {
		g_regex_unref(gs->any);
		gs->any = NULL;
	}
	if (gs->dir) {
		g_regex_unref(gs->dir);
		gs->dir = NULL;
	}

	gs->any = compile_combined(gs, false, err);
	if (*err)
		return 1;
	gs->dir = compile_combined(gs, true, err);
	if (*err)
		return 1;

	gs->compiled = true;
	return 0;
}

/*
 * Which pattern matched? Only called once the combined regex has already said
 * yes, so the linear scan runs at most once per excluded path.
 */
static struct glob_pat *attribute(struct glob_set *gs, const char *path,
				  bool is_dir)
{
	for (unsigned int i = 0; i < gs->pats->len; i++) {
		struct glob_pat *gp = g_ptr_array_index(gs->pats, i);

		if (!gp->re || (gp->dir_only && !is_dir))
			continue;
		if (g_regex_match(gp->re, path, 0, NULL))
			return gp;
	}
	return NULL;
}

bool glob_set_match(struct glob_set *gs, const char *path, bool is_dir,
		    const char **which)
{
	struct glob_pat *gp;

	if (gs->pats->len == 0)
		return false;

	gp = g_hash_table_lookup(gs->literals, path);
	if (!gp) {
		bool hit = (gs->any && g_regex_match(gs->any, path, 0, NULL)) ||
			   (is_dir && gs->dir &&
			    g_regex_match(gs->dir, path, 0, NULL));

		if (!hit)
			return false;
		gp = attribute(gs, path, is_dir);
	}

	if (gp) {
		gp->matches++;
		if (which)
			*which = gp->pattern;
	}
	return true;
}

bool glob_set_stat(const struct glob_set *gs, unsigned int i,
		   const char **pattern, unsigned long *matches)
{
	unsigned int seen = 0;

	for (unsigned int j = 0; j < gs->pats->len; j++) {
		struct glob_pat *gp = g_ptr_array_index(gs->pats, j);

		if (gp->internal)
			continue;	/* the hashfile and its sidecars */
		if (seen++ != i)
			continue;
		*pattern = gp->pattern;
		*matches = gp->matches;
		return true;
	}
	return false;
}
