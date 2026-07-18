/*
 * autotune.c
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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>

#include "autotune.h"
#include "dbfile.h"
#include "debug.h"
#include "opt.h"
#include "util.h"

/* Keep each trial short: sample at most this many files / this many bytes. */
#define AT_DEFAULT_MAX_FILES	20000ULL
#define AT_DEFAULT_MAX_BYTES	(8ULL << 30)	/* 8 GiB */
/* Repeat the whole interleaved sweep this many times; keep each candidate's
 * fastest run (least perturbed by transient load). */
#define AT_DEFAULT_ROUNDS	3

struct sample {
	GPtrArray	*paths;		/* char* full paths (owned) */
	uint64_t	total_bytes;
	uint64_t	max_files;
	uint64_t	max_bytes;
};

static bool sample_full(const struct sample *s)
{
	return s->paths->len >= s->max_files ||
	       s->total_bytes >= s->max_bytes;
}

static void sample_add(struct sample *s, const char *path, uint64_t size)
{
	g_ptr_array_add(s->paths, strdup(path));
	s->total_bytes += size;
}

/* Recursively collect regular files under `dir` until the sample is full. */
static void collect_dir(struct sample *s, const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;

	if (!d)
		return;

	while (!sample_full(s) && (de = readdir(d))) {
		char child[PATH_MAX];
		struct stat st;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if ((size_t)snprintf(child, sizeof(child), "%s/%s", dir,
				     de->d_name) >= sizeof(child))
			continue;
		/* Don't follow symlinks: mirrors the scan and avoids loops. */
		if (lstat(child, &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			if (options.recurse_dirs)
				collect_dir(s, child);
		} else if (S_ISREG(st.st_mode)) {
			if ((uint64_t)st.st_size < options.min_filesize)
				continue;
			sample_add(s, child, st.st_size);
		}
	}
	closedir(d);
}

static void collect_sample(struct sample *s, char **roots, int nroots)
{
	for (int i = 0; i < nroots && !sample_full(s); i++) {
		struct stat st;

		if (lstat(roots[i], &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			collect_dir(s, roots[i]);
		else if (S_ISREG(st.st_mode) &&
			 (uint64_t)st.st_size >= options.min_filesize)
			sample_add(s, roots[i], st.st_size);
	}
}

static uint64_t env_u64(const char *name, uint64_t def)
{
	const char *v = getenv(name);
	char *end;
	unsigned long long n;

	if (!v || !*v)
		return def;
	n = strtoull(v, &end, 10);
	if (*end != '\0' || n == 0)
		return def;
	return n;
}

/* Candidate io-thread counts to try, filtered to a sensible ceiling. */
static unsigned int build_candidates(unsigned int *out, unsigned int out_len)
{
	static const unsigned int base[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
	unsigned int ncpus = get_num_cpus();
	unsigned int maxc = 2 * ncpus;
	unsigned int n = 0;

	if (maxc < 8)
		maxc = 8;	/* always probe past the old cap of 8 */
	if (maxc > 16)
		maxc = 16;

	for (size_t i = 0; i < ARRAY_SIZE(base) && n < out_len; i++)
		if (base[i] <= maxc)
			out[n++] = base[i];
	return n;
}

static double ts_diff(struct timespec a, struct timespec b)
{
	return (double)(b.tv_sec - a.tv_sec) +
	       (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

/*
 * Flush and drop the page cache so the next trial reads cold. Returns whether
 * it actually dropped (needs root); when it can't, callers skip it entirely
 * rather than paying a system-wide sync() that drops nothing.
 */
static bool drop_caches(void)
{
	int fd = open("/proc/sys/vm/drop_caches", O_WRONLY | O_CLOEXEC);
	ssize_t w;

	if (fd < 0)
		return false;
	sync();
	w = write(fd, "3\n", 2);	/* best effort */
	(void)w;
	close(fd);
	return true;
}

/*
 * Run one trial: re-exec oans on the sample (fed on stdin as a file list) with
 * a fixed --io-threads and no hashfile, so it only reads and hashes in memory.
 * Returns wall-clock seconds, or a negative value if the child failed.
 */
static double run_trial(const char *self, const char *listpath, unsigned int k)
{
	_cleanup_(closefd) int fd_list = open(listpath, O_RDONLY | O_CLOEXEC);
	struct timespec t0, t1;
	char kbuf[32];
	pid_t pid;
	int status;

	if (fd_list < 0)
		return -1.0;

	snprintf(kbuf, sizeof(kbuf), "--io-threads=%u", k);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	pid = fork();
	if (pid < 0)
		return -1.0;

	if (pid == 0) {
		int devnull = open("/dev/null", O_WRONLY);
		char *argv[] = { (char *)self, (char *)"--quiet", kbuf,
				 (char *)"-", NULL };

		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
		}
		dup2(fd_list, STDIN_FILENO);
		execv(self, argv);
		_exit(127);
	}

	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	clock_gettime(CLOCK_MONOTONIC, &t1);

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1.0;
	return ts_diff(t0, t1);
}

static void persist(unsigned int k)
{
	struct dbhandle *db;

	if (!options.hashfile)
		return;

	db = dbfile_open_handle(options.hashfile);
	if (!db) {
		printf("  (could not open %s to store the result)\n",
		       options.hashfile);
		return;
	}
	if (dbfile_set_config_int(db, AUTOTUNE_CONFIG_KEY, k) == 0)
		printf("Stored in the hashfile; future runs on %s use "
		       "--io-threads=%u automatically.\n", options.hashfile, k);
	dbfile_close_handle(db);
}

int autotune_run(char **roots, int nroots)
{
	struct sample s = {
		.paths = g_ptr_array_new_with_free_func(free),
		.max_files = env_u64("DUPEREMOVE_AUTOTUNE_MAX_FILES",
				     AT_DEFAULT_MAX_FILES),
		.max_bytes = env_u64("DUPEREMOVE_AUTOTUNE_MAX_BYTES",
				     AT_DEFAULT_MAX_BYTES),
	};
	unsigned int rounds = env_u64("DUPEREMOVE_AUTOTUNE_ROUNDS",
				      AT_DEFAULT_ROUNDS);
	unsigned int cand[16];
	unsigned int ncand;
	double *best;
	char self[PATH_MAX];
	char listpath[] = "/tmp/oans-autotune.XXXXXX";
	ssize_t self_len;
	unsigned int best_k = 0;
	double best_time = 0;
	bool cold;
	int listfd, ret = 0;

	self_len = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (self_len <= 0) {
		eprintf("autotune: cannot find my own executable path\n");
		ret = 1;
		goto out_paths;
	}
	self[self_len] = '\0';

	printf("Autotuning --io-threads on %d path%s...\n", nroots,
	       nroots == 1 ? "" : "s");
	collect_sample(&s, roots, nroots);
	if (s.paths->len == 0) {
		eprintf("autotune: found no regular files to measure with.\n");
		ret = 1;
		goto out_paths;
	}
	printf("Sample: %u files, %s\n", s.paths->len,
	       human_size(s.total_bytes));

	/* Write the sample as a newline-separated file list for the trials. */
	listfd = mkstemp(listpath);
	if (listfd < 0) {
		eprintf("autotune: cannot create temp file list: %s\n",
			strerror(errno));
		ret = 1;
		goto out_paths;
	}
	{
		FILE *lf = fdopen(listfd, "w");

		if (!lf) {
			close(listfd);
			ret = 1;
			goto out_list;
		}
		for (guint i = 0; i < s.paths->len; i++)
			fprintf(lf, "%s\n", (char *)s.paths->pdata[i]);
		fclose(lf);
	}

	ncand = build_candidates(cand, ARRAY_SIZE(cand));
	best = calloc(ncand, sizeof(*best));
	abort_on(!best);

	/* Probe once (this also primes a cold cache before the first trial). */
	cold = drop_caches();
	if (cold)
		printf("Dropping page cache between trials (cold reads).\n");
	else
		printf("Note: cannot drop page cache (run as root for cold-read "
		       "numbers); results may reflect a warm cache.\n");
	printf("Running %u round%s over %u thread setting%s...\n\n",
	       rounds, rounds == 1 ? "" : "s", ncand, ncand == 1 ? "" : "s");

	for (unsigned int r = 0; r < rounds; r++) {
		for (unsigned int i = 0; i < ncand; i++) {
			double t;

			if (cold)
				drop_caches();
			t = run_trial(self, listpath, cand[i]);
			if (t < 0)
				continue;
			if (best[i] == 0 || t < best[i])
				best[i] = t;
		}
	}

	/* Find the fastest candidate, then print every row in candidate order. */
	for (unsigned int i = 0; i < ncand; i++)
		if (best[i] != 0 && (best_time == 0 || best[i] < best_time)) {
			best_time = best[i];
			best_k = cand[i];
		}

	printf("  %-9s %-10s %s\n", "threads", "time", "throughput");
	for (unsigned int i = 0; i < ncand; i++) {
		char tp[40];

		if (best[i] == 0) {
			printf("  %-9u %-10s %s\n", cand[i], "failed", "-");
			continue;
		}
		snprintf(tp, sizeof(tp), "%s/s",
			 human_size((uint64_t)(s.total_bytes / best[i])));
		printf("  %-9u %-10s %-12s%s\n", cand[i],
		       human_duration(best[i]), tp,
		       cand[i] == best_k ? "  <- best" : "");
	}

	if (best_k == 0) {
		eprintf("\nautotune: every trial failed; nothing to recommend.\n");
		ret = 1;
		goto out_best;
	}

	printf("\nRecommended: --io-threads=%u\n", best_k);
	persist(best_k);

out_best:
	free(best);
out_list:
	unlink(listpath);
out_paths:
	g_ptr_array_free(s.paths, TRUE);
	return ret;
}
