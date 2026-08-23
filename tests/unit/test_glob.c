/*
 * --exclude matching, in .gitignore syntax.
 *
 * Its own translation unit. The sources below are #included rather than
 * linked, because tests here call their static functions; every other source
 * the suite needs is compiled once and linked, which is what makes a mutant
 * rebuild one subject instead of all of them.
 */
static bool gs_hit(const char *pattern, const char *path, bool is_dir)
{
	char *err = NULL;
	struct glob_set *gs = glob_set_new();
	bool r;

	if (glob_set_add(gs, pattern, &err) || glob_set_compile(gs, &err)) {
		fprintf(stderr, "glob pattern \"%s\" rejected: %s\n", pattern, err);
		abort();
	}
	r = glob_set_match(gs, path, is_dir, NULL);
	glob_set_free(gs);
	return r;
}

MU_TEST(test_glob_basename) {
	/* The #147 case: a bare name matches at any depth. Under the old
	 * full-path fnmatch these all silently matched nothing. */
	mu_check(gs_hit("@eaDir", "/srv/media/@eaDir", true));
	mu_check(gs_hit("@eaDir", "/srv/a/b/c/@eaDir", true));
	mu_check(gs_hit("node_modules", "/home/u/p/node_modules", true));
	/* ...but only whole components. */
	mu_check(!gs_hit("@eaDir", "/srv/media/@eaDirectory", true));
	mu_check(!gs_hit("@eaDir", "/srv/media/x@eaDir", true));
	mu_check(!gs_hit("node_modules", "/home/u/node_modules_old", true));

	mu_check(gs_hit("*.iso", "/data/img/x.iso", false));
	mu_check(!gs_hit("*.iso", "/data/img/x.iso.part", false));
}

MU_TEST(test_glob_anchored_vs_any_depth) {
	/* Leading '/' anchors at the filesystem root. */
	mu_check(gs_hit("/srv/media/cache*", "/srv/media/cache1", false));
	mu_check(!gs_hit("/srv/media/cache*", "/other/srv/media/cache1", false));

	/* An interior '/' matches at any depth. */
	mu_check(gs_hit("Steam/temp", "/data/Steam/temp", true));
	mu_check(gs_hit("Steam/temp", "/home/u/games/Steam/temp", true));
	mu_check(!gs_hit("Steam/temp", "/data/Steamx/temp", true));
}

MU_TEST(test_glob_wildcards_respect_separators) {
	/* '*' must not cross a '/'. */
	mu_check(gs_hit("/a/*", "/a/b", false));
	mu_check(!gs_hit("/a/*", "/a/b/c", false));

	/* '**' does cross. */
	mu_check(gs_hit("/a/**/t", "/a/t", false));
	mu_check(gs_hit("/a/**/t", "/a/b/t", false));
	mu_check(gs_hit("/a/**/t", "/a/b/c/d/t", false));

	/* '?' is exactly one non-separator. */
	mu_check(gs_hit("a?.txt", "/x/ab.txt", false));
	mu_check(!gs_hit("a?.txt", "/x/abc.txt", false));
	mu_check(!gs_hit("a?.txt", "/x/a/.txt", false));
}

/*
 * `**` not followed by a separator, which is a different branch of
 * glob_to_regex from the `**\/` above and had no test at all - the mutation
 * sweep found nine survivors on those two lines.
 *
 * It is also where this implementation and gitignore disagree, so these cases
 * are as much a record of the disagreement as a check on it. glob.h states two
 * rules that collide - a pattern with no '/' is a basename rule, and `**`
 * crosses directory boundaries - and here the second wins. git resolves it the
 * other way: in gitignore `**` is only special as a whole path component, and
 * inside one it means `*`. Nobody writes `node**` on purpose and the
 * difference only ever excludes more than was asked for, so what is pinned
 * here is what the code does, not what it arguably should.
 */
MU_TEST(test_glob_double_star_without_a_separator) {
	/* Trailing: everything below the named directory, and the directory
	 * itself only if something follows the stars. */
	mu_check(gs_hit("/a/**", "/a/b", false));
	mu_check(gs_hit("/a/**", "/a/b/c/d", false));

	/* Interior, inside one component: the stars cross '/' where a single
	 * '*' would not. This is the divergence from gitignore. */
	mu_check(gs_hit("/a**z", "/ab/cd/z", false));
	mu_check(!gs_hit("/a*z", "/ab/cd/z", false));

	/* And so a bare pattern with '**' stops being purely a basename rule:
	 * it matches through the separator on the left. */
	mu_check(gs_hit("a**", "/a/bbc", false));
	mu_check(!gs_hit("a**", "/bbc", false));

	/* Three or more stars are the same as two - the run is counted, not
	 * matched pairwise. */
	mu_check(gs_hit("/a/***/t", "/a/b/c/t", false));
}

/*
 * A backslash escapes the next character, so a pattern can name a file that
 * has a metacharacter in it. One line of glob_to_regex, no test, and nine
 * surviving mutants on it - including the `i + 1 < len` bound, whose failure
 * is a read one past the end of the pattern.
 */
MU_TEST(test_glob_backslash_escapes) {
	/* An escaped wildcard is a literal, and stops being a wildcard. */
	mu_check(gs_hit("a\\*b", "/x/a*b", false));
	mu_check(!gs_hit("a\\*b", "/x/axxb", false));

	/* Same for the other metacharacters, so a real name gets named. */
	mu_check(gs_hit("a\\?b", "/x/a?b", false));
	mu_check(!gs_hit("a\\?b", "/x/azb", false));
	mu_check(gs_hit("db\\[1].hash", "/var/db[1].hash", false));

	/* The escape consumes exactly one character; what follows is ordinary
	 * again. */
	mu_check(gs_hit("a\\**b", "/x/a*zzb", false));

	/* A trailing backslash has nothing to escape. It must be treated as a
	 * literal rather than reaching past the end of the pattern for a
	 * character that is not there. */
	mu_check(gs_hit("a\\", "/x/a\\", false));
	mu_check(!gs_hit("a\\", "/x/ab", false));
}

MU_TEST(test_glob_character_classes) {
	mu_check(gs_hit("f[0-9].log", "/x/f3.log", false));
	mu_check(!gs_hit("f[0-9].log", "/x/fx.log", false));
	mu_check(gs_hit("f[!0-9].log", "/x/fx.log", false));
	mu_check(!gs_hit("f[!0-9].log", "/x/f3.log", false));
	mu_check(gs_hit("f[abc].log", "/x/fb.log", false));

	/* A '.' in the pattern is a literal, not "any character". */
	mu_check(!gs_hit("a.txt", "/x/axtxt", false));
}

MU_TEST(test_glob_directory_only) {
	/* A trailing '/' restricts the pattern to directories. */
	mu_check(gs_hit("cache/", "/a/cache", true));
	mu_check(!gs_hit("cache/", "/a/cache", false));
	/* Without it, either kind matches. */
	mu_check(gs_hit("cache", "/a/cache", true));
	mu_check(gs_hit("cache", "/a/cache", false));
}

MU_TEST(test_glob_literal_paths_are_not_globs) {
	/* An exact path oans excludes on the user's behalf must match itself
	 * even when it contains regex/glob metacharacters. */
	char *err = NULL;
	struct glob_set *gs = glob_set_new();

	glob_set_add_literal(gs, "/tmp/h[1].db");
	mu_check(glob_set_compile(gs, &err) == 0);
	mu_check(glob_set_match(gs, "/tmp/h[1].db", false, NULL));
	mu_check(!glob_set_match(gs, "/tmp/h1.db", false, NULL));
	glob_set_free(gs);
}

MU_TEST(test_glob_reports_matching_pattern_and_counts) {
	char *err = NULL;
	struct glob_set *gs = glob_set_new();
	const char *which = NULL, *pat = NULL;
	bool matched = false;

	mu_check(glob_set_add(gs, "*.log", &err) == 0);
	mu_check(glob_set_add(gs, "@eaDir", &err) == 0);
	mu_check(glob_set_compile(gs, &err) == 0);

	mu_check(glob_set_match(gs, "/a/b/x.log", false, &which));
	mu_check(which && strcmp(which, "*.log") == 0);

	mu_check(glob_set_match(gs, "/a/@eaDir", true, &which));
	mu_check(which && strcmp(which, "@eaDir") == 0);

	/* Per-pattern flags back the "matched nothing" warning (#147). */
	mu_check(glob_set_stat(gs, 0, &pat, &matched) && matched);
	mu_check(glob_set_stat(gs, 1, &pat, &matched) && matched);
	mu_check(!glob_set_stat(gs, 2, &pat, &matched));
	glob_set_free(gs);
}

MU_TEST(test_glob_rejects_malformed) {
	char *err = NULL;
	struct glob_set *gs = glob_set_new();

	mu_check(glob_set_add(gs, "f[abc", &err) != 0);
	mu_check(err != NULL);
	g_free(err);
	glob_set_free(gs);
}

MU_TEST(test_glob_empty_set_matches_nothing) {
	char *err = NULL;
	struct glob_set *gs = glob_set_new();

	mu_check(glob_set_compile(gs, &err) == 0);
	mu_check(!glob_set_match(gs, "/anything", false, NULL));
	glob_set_free(gs);
}

/*
 * The property the whole of #159 rests on: hashing a byte stream in one go and
 * hashing it across a save/restore must give the same digest. If it ever did
 * not, a resumed scan would store a digest matching nothing - silently, since
 * nothing downstream can tell a wrong digest from a file that simply has no
 * duplicate.
 */
#define PROP_GLOB_PATHS 8
static void gen_glob_segment(struct prop *p, char *buf, size_t sz)
{
	size_t len = (size_t)prop_range(p, 1, sz - 1);

	for (size_t i = 0; i < len; i++) {
		switch (prop_below(p, 8)) {
		case 0: buf[i] = '*'; break;
		case 1: buf[i] = '?'; break;
		case 2: buf[i] = '.'; break;
		/* '[' and ']' reach append_class(), and an unmatched '[' is the
		 * one way a pattern can fail to compile - which is what makes
		 * gs_of()'s rejection path live. Without them it was dead:
		 * measured 0 rejections in 12,000 compiles, so both the class
		 * parser and the malformed-pattern branch were sampled zero
		 * times while the comment claimed otherwise. */
		case 3: buf[i] = '['; break;
		case 4: buf[i] = ']'; break;
		default: buf[i] = (char)prop_range(p, 'a', 'c'); break;
		}
	}
	buf[len] = '\0';
}

static void gen_glob_path(struct prop *p, char *buf, size_t sz)
{
	unsigned int segs = (unsigned int)prop_range(p, 1, 3);
	size_t o = 0;

	for (unsigned int s = 0; s < segs && o + 6 < sz; s++) {
		size_t len = (size_t)prop_range(p, 1, 3);

		buf[o++] = '/';
		for (size_t i = 0; i < len; i++)
			buf[o++] = prop_chance(p, 6) ? '.'
						     : (char)prop_range(p, 'a', 'c');
	}
	buf[o] = '\0';
}

/* A literal cannot be malformed, so a failure here is a bug in the test, not
 * an input worth skipping - which is why this aborts where gs_of() returns. */
static struct glob_set *gs_literal(const char *path)
{
	struct glob_set *gs = glob_set_new();
	char *err = NULL;

	glob_set_add_literal(gs, path);
	if (glob_set_compile(gs, &err)) {
		fprintf(stderr, "literal \"%s\" rejected: %s\n", path, err);
		abort();
	}
	return gs;
}

/*
 * Build a set from `n` patterns, or NULL if any of them is malformed - which
 * the generator does produce, since '[' comes out of the same draw as the rest.
 * A rejected pattern must not be laundered into "matches nothing": that would
 * make every property below pass vacuously on exactly the inputs where the
 * compiler had something to say.
 */
static struct glob_set *gs_of(const char *const *pats, unsigned int n)
{
	struct glob_set *gs = glob_set_new();
	char *err = NULL;

	for (unsigned int i = 0; i < n; i++) {
		if (glob_set_add(gs, pats[i], &err)) {
			g_free(err);
			glob_set_free(gs);
			return NULL;
		}
	}
	if (glob_set_compile(gs, &err)) {
		g_free(err);
		glob_set_free(gs);
		return NULL;
	}
	return gs;
}

/*
 * The rule the 1.6.0 break was made for (#147): a pattern with no '/' is about
 * the basename and nothing else. Stated without reimplementing the matcher -
 * the path and its own last component have to give the same answer, whatever
 * that answer is - so this cannot pass by agreeing with a second copy of the
 * same bug.
 *
 * `**` is excluded from the pattern, and finding out why is what this property
 * was worth writing for. glob.h states two rules that collide: a pattern with
 * no '/' is a basename rule, and `**` crosses directory boundaries. Here the
 * second wins, so `a**` matches `/a/bbc` - crossing the slash, against a
 * basename that is `bbc` - while `/bbc` alone does not match. git resolves the
 * same collision the other way: in gitignore `**` is only special as a whole
 * path component, and inside one it means `*`. Nobody writes `node**` on
 * purpose and the divergence only ever excludes more than was asked for, so it
 * is left alone here rather than fixed in a test commit; excluded by name so
 * that the property states what is actually promised.
 */
static void squash_doublestars(char *s)
{
	char *w = s;

	for (const char *r = s; *r; r++)
		if (*r != '*' || w == s || w[-1] != '*')
			*w++ = *r;
	*w = '\0';
}

MU_TEST(test_prop_a_bare_pattern_reads_only_the_basename) {
	declare_prop(p, 3000);

	if (prop_skip_jit(__func__))
		return;

	while (prop_next(&p)) {
		char pat[6];
		const char *pats[] = { pat };
		struct glob_set *gs;

		gen_glob_segment(&p, pat, sizeof(pat));
		squash_doublestars(pat);
		gs = gs_of(pats, 1);
		if (!gs)
			continue;

		/* Several paths per compile: building the automaton is nearly
		 * all of what a case costs here - a GRegex compile against a
		 * matcher call - and paths are the axis worth sampling. */
		for (unsigned int k = 0; k < PROP_GLOB_PATHS; k++) {
			char path[24], base[sizeof(path) + 1];
			bool is_dir = prop_bool(&p);
			const char *slash;

			gen_glob_path(&p, path, sizeof(path));
			slash = strrchr(path, '/');
			snprintf(base, sizeof(base), "/%s",
				 slash ? slash + 1 : path);
			prop_check(&p, glob_set_match(gs, path, is_dir, NULL) ==
				       glob_set_match(gs, base, is_dir, NULL));
		}
		glob_set_free(gs);
	}
}

/*
 * --exclude is a union, so adding a pattern can only ever exclude more. The
 * failure this rules out is a shared automaton in which one pattern's
 * compilation changes another's - the patterns are joined into a single
 * regex, and a fragment that leaks a group or an alternation past its own
 * boundary would do exactly that.
 */
MU_TEST(test_prop_adding_a_pattern_never_unexcludes_a_path) {
	declare_prop(p, 3000);

	if (prop_skip_jit(__func__))
		return;

	while (prop_next(&p)) {
		char a[6], b[6];
		const char *one[] = { a };
		const char *both[] = { a, b };
		struct glob_set *gs1, *gs2;

		gen_glob_segment(&p, a, sizeof(a));
		gen_glob_segment(&p, b, sizeof(b));
		gs1 = gs_of(one, 1);
		gs2 = gs_of(both, 2);
		if (!gs1 || !gs2) {
			glob_set_free(gs1);
			glob_set_free(gs2);
			continue;
		}
		for (unsigned int k = 0; k < PROP_GLOB_PATHS; k++) {
			char path[24];
			bool is_dir = prop_bool(&p);

			gen_glob_path(&p, path, sizeof(path));
			if (glob_set_match(gs1, path, is_dir, NULL))
				prop_check(&p, glob_set_match(gs2, path, is_dir, NULL));
		}
		glob_set_free(gs1);
		glob_set_free(gs2);
	}
}

/*
 * A trailing '/' restricts a pattern to directories, and the walk hands
 * `is_dir` straight from the stat. Getting this backwards would exclude every
 * *file* a `cache/` pattern named and none of the directories - and the run
 * would look like it worked, since something was excluded.
 */
MU_TEST(test_prop_a_directory_pattern_never_matches_a_file) {
	declare_prop(p, 3000);

	if (prop_skip_jit(__func__))
		return;

	while (prop_next(&p)) {
		char seg[6], pat[sizeof(seg) + 2];
		const char *pats[] = { pat };
		struct glob_set *gs;

		gen_glob_segment(&p, seg, sizeof(seg));
		snprintf(pat, sizeof(pat), "%s/", seg);
		gs = gs_of(pats, 1);
		if (!gs)
			continue;
		for (unsigned int k = 0; k < PROP_GLOB_PATHS; k++) {
			char path[24];

			gen_glob_path(&p, path, sizeof(path));
			prop_check(&p, !glob_set_match(gs, path, false, NULL));
		}
		glob_set_free(gs);
	}
}

/*
 * A literal is exempt from metacharacter interpretation, which is what keeps a
 * hashfile called `db[1].hash` from being read as a character class - oans adds
 * its own hashfile and the two WAL sidecars this way. So it must match that one
 * path and, being an absolute path rather than a basename rule, nothing else.
 */
MU_TEST(test_prop_a_literal_path_matches_itself_and_nothing_else) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char lit[24], other[24];
		struct glob_set *gs;
		bool is_dir = prop_bool(&p);
		size_t n;

		gen_glob_path(&p, lit, sizeof(lit));
		/* Metacharacters in the *literal*, which is the case it exists
		 * for: as a pattern this would be a class or a wildcard. */
		n = strlen(lit);
		if (n + 3 < sizeof(lit) && prop_bool(&p)) {
			lit[n] = prop_bool(&p) ? '[' : '*';
			lit[n + 1] = 'a';
			lit[n + 2] = '\0';
		}
		gen_glob_path(&p, other, sizeof(other));

		gs = gs_literal(lit);
		prop_check(&p, glob_set_match(gs, lit, is_dir, NULL));
		if (strcmp(lit, other))
			prop_check(&p, !glob_set_match(gs, other, is_dir, NULL));
		glob_set_free(gs);
	}
}

/*
 * The FIDEDUPERANGE probe's answer taxonomy (#224). Worth testing directly and
 * exhaustively: the probe's whole job is turning one ioctl result into a
 * verdict, and what a filesystem returns is exactly what a test host cannot
 * vary. Both wrong answers are bad in different ways -- a wrong NO silently
 * skips a whole tree, a wrong YES brings back per-file dedupe errors -- so
 * anything that is not a clear statement about the filesystem stays UNKNOWN.
 */
/*
 * ---------------------------------------------------------------------------
 * Properties of the hashfile
 *
 * The tables above name a layout and its answer. These name the two
 * relationships that decide whether a snapshot-aware scan is safe at all, and
 * go looking for a layout that breaks them - which is the right shape here,
 * because what makes a layout interesting (a hole, two records that abut, a
 * split at a block boundary) is a fact about extents rather than one anybody
 * sits down and enumerates.
 * ---------------------------------------------------------------------------
 */

/* One donor per case, since a file may hold only one set of extent rows. */
