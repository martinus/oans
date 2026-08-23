/*
 * Paths past PATH_MAX (#117).
 *
 * Part of the oans unit suite. tests/unit/main.c includes this file along
 * with the sources it exercises, so a test still reaches a static function
 * the way it always did.
 */

#define LP_COMP_LEN 255
/* absdir runs to ~PATH_MAX + 383; a leaf adds at most two more
 * NAME_MAX components on top of that. */
#define LP_PATH_BUF	6144

static void lp_fill(char *buf, char c, int n)
{
	memset(buf, c, n);
	buf[n] = '\0';
}

/*
 * Build the deep tree. *out_levels counts the directories created so far and is
 * updated as we descend, so a failure partway still leaves the caller enough
 * state to tear the tree down (we are chdir'd into it and cannot name it with
 * an absolute path). base_out is emptied first for the same reason.
 */
static int lp_make_deep(char *absdir, size_t abscap, char *base_out,
			size_t base_cap, int *out_levels, const char *victim,
			const char *contents)
{
	char comp[LP_COMP_LEN + 1];
	char base[] = "/tmp/oans-longpath-XXXXXX";
	size_t len = strlen(base);
	int fd;

	lp_fill(comp, 'd', LP_COMP_LEN);
	base_out[0] = '\0';
	*out_levels = 0;

	if (!mkdtemp(base) || len + 1 > base_cap || len + 1 > abscap)
		return -1;
	memcpy(base_out, base, len + 1);
	if (chdir(base) != 0)
		return -1;
	memcpy(absdir, base, len + 1);

	/* Descend until the directory path alone exceeds PATH_MAX, so the walk
	 * exercises the multi-chunk openat chain (not just one openat). */
	while (len < (size_t)PATH_MAX + 128) {
		if (mkdir(comp, 0700) != 0 || chdir(comp) != 0)
			return -1;
		(*out_levels)++;
		if (len + 1 + LP_COMP_LEN + 1 > abscap)
			return -1;
		absdir[len++] = '/';
		memcpy(absdir + len, comp, LP_COMP_LEN + 1);
		len += LP_COMP_LEN;
	}

	fd = open(victim, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	if (write(fd, contents, strlen(contents)) != (ssize_t)strlen(contents)) {
		close(fd);
		return -1;
	}
	close(fd);

	return 0;
}

static void lp_destroy_deep(int savedcwd, const char *base, const char *victim,
			    int levels)
{
	char comp[LP_COMP_LEN + 1];
	int i;

	lp_fill(comp, 'd', LP_COMP_LEN);

	/* CWD is the leaf dir: drop the file, then climb + rmdir each level. */
	unlink(victim);
	for (i = 0; i < levels; i++) {
		if (chdir("..") != 0)
			break;
		rmdir(comp);
	}
	if (savedcwd >= 0 && fchdir(savedcwd) != 0)
		return;
	if (base[0])
		rmdir(base);
}

/*
 * The assertions, split out of test_longpath() below: mu_check() returns from
 * its enclosing function on failure, so keeping them here means a failure can
 * never skip the teardown -- which would otherwise strand the process CWD
 * inside a directory too deep to name and leak the tree under /tmp.
 */
static void lp_check_helpers(const char *absdir, const char *base,
			     const char *victim, const char *contents)
{
	char leaf[LP_PATH_BUF];
	struct stat st;
	int fd, bfd, n;
	char buf[128] = { 0 };
	ssize_t r;
	DIR *d;
	struct dirent *de;
	bool found = false;

	/* The directory itself is past PATH_MAX, forcing the chunked walk. */
	mu_check(strlen(absdir) > PATH_MAX);

	n = snprintf(leaf, sizeof(leaf), "%s/%s", absdir, victim);
	mu_check(n > 0 && (size_t)n < sizeof(leaf));
	mu_check(strlen(leaf) > PATH_MAX);

	/* 1. open + read the deep file. */
	fd = longpath_open(leaf, O_RDONLY);
	mu_check(fd >= 0);
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	mu_check(r == (ssize_t)strlen(contents));
	mu_check(strcmp(buf, contents) == 0);

	/* 2. stat + lstat the deep file. */
	mu_check(longpath_stat(leaf, &st) == 0);
	mu_check((size_t)st.st_size == strlen(contents));
	memset(&st, 0, sizeof(st));
	mu_check(longpath_lstat(leaf, &st) == 0);
	mu_check((size_t)st.st_size == strlen(contents));

	/* 3. opendir the deep directory and list it. */
	d = longpath_opendir(absdir);
	mu_check(d != NULL);
	while ((de = readdir(d))) {
		if (strcmp(de->d_name, victim) == 0) {
			found = true;
			break;
		}
	}
	closedir(d);
	mu_check(found);

	/* 4a. missing final component → ENOENT, not ENAMETOOLONG. */
	{
		char missname[LP_COMP_LEN + 1];
		char miss_leaf[LP_PATH_BUF];

		lp_fill(missname, 'x', LP_COMP_LEN);
		snprintf(miss_leaf, sizeof(miss_leaf), "%s/%s", absdir, missname);
		mu_check(strlen(miss_leaf) > PATH_MAX);
		errno = 0;
		mu_check(longpath_open(miss_leaf, O_RDONLY) < 0);
		mu_check(errno == ENOENT);
		errno = 0;
		mu_check(longpath_stat(miss_leaf, &st) < 0);
		mu_check(errno == ENOENT);
	}

	/* 4b. missing intermediate directory → ENOENT from the ancestor walk. */
	{
		char missdir[LP_COMP_LEN + 1];
		char miss_mid[LP_PATH_BUF];

		lp_fill(missdir, 'z', LP_COMP_LEN);
		snprintf(miss_mid, sizeof(miss_mid), "%s/%s/%s", absdir,
			 missdir, victim);
		mu_check(strlen(miss_mid) > PATH_MAX);
		errno = 0;
		mu_check(longpath_open(miss_mid, O_RDONLY) < 0);
		mu_check(errno == ENOENT);
	}

	/* 5. short path: identical to plain open(). */
	bfd = longpath_open(base, O_RDONLY | O_DIRECTORY);
	mu_check(bfd >= 0);
	close(bfd);
}

/*
 * --- the option parser and the human-readable formatters (util.c) ---
 *
 * These had no unit test at all, which the mutation sweep found rather than
 * anyone noticing: 113 of src/util.c's 203 surviving mutants were in these four
 * functions, against 5-9% survival in the escaping code next door. That is not
 * a weak test, it is an absent one, and the shape of what it lets through is
 * the worrying part - `parse_size` is a ladder of fallthroughs, so dropping one
 * `mult *= 1024` makes `--max-filesize=10G` mean ten megabytes, silently, on a
 * run that otherwise looks exactly right.
 */

/* parse_size takes a mutable string; the option parser hands it argv. */
MU_TEST(test_longpath) {
	const char *contents = "over-the-PATH_MAX limit\n";
	char victim[LP_COMP_LEN + 1];
	char absdir[LP_PATH_BUF];
	char base[64] = { 0 };
	int levels = 0;
	int savedcwd = open(".", O_PATH | O_CLOEXEC);
	int rc = -1;

	lp_fill(victim, 'v', LP_COMP_LEN);

	/*
	 * Build, check, tear down -- then assert. Teardown must run before any
	 * mu_check() that could return early, and we only descend at all once we
	 * hold an fd we can fchdir() back to.
	 */
	if (savedcwd >= 0) {
		rc = lp_make_deep(absdir, sizeof(absdir), base, sizeof(base),
				  &levels, victim, contents);
		if (rc == 0)
			lp_check_helpers(absdir, base, victim, contents);
		lp_destroy_deep(savedcwd, base, victim, levels);
		close(savedcwd);
	}

	mu_check(savedcwd >= 0);
	mu_check(rc == 0);
}


/* --- gitignore-style --exclude matching (glob.c) --- */

/*
 * Match one path against one pattern. A pattern that fails to compile aborts
 * rather than returning false: laundering it into "no match" would let every
 * negative assertion below pass vacuously.
 */
