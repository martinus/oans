/*
 * The live progress block.
 *
 * Its own translation unit. The sources below are #included rather than
 * linked, because tests here call their static functions; every other source
 * the suite needs is compiled once and linked, which is what makes a mutant
 * rebuild one subject instead of all of them.
 */
MU_TEST(test_progress_copy_path) {
	char buf[32];

	/* Fits: copied verbatim. */
	progress_copy_path(buf, sizeof(buf), "abc");
	mu_check(strcmp(buf, "abc") == 0);

	/* Exactly filling the buffer (len == cap) must still elide. */
	progress_copy_path(buf, 4, "abcd");
	mu_check(strlen(buf) < 4);

	/*
	 * Too long: elided with the renderer's single "…", keeping the real
	 * head and the basename so both ends stay readable.
	 */
	progress_copy_path(buf, sizeof(buf),
			   "/a/very/long/directory/path/basename.txt");
	mu_check(strlen(buf) < sizeof(buf));
	mu_check(strstr(buf, "…") != NULL);
	mu_check(buf[0] == '/');                       /* real head kept */
	mu_check(strstr(buf, "name.txt") != NULL);     /* basename kept */

	/* Degenerate caps must stay in bounds and NUL-terminated. */
	for (size_t cap = 1; cap <= sizeof(buf); cap++) {
		memset(buf, 'X', sizeof(buf));
		progress_copy_path(buf, cap, "/some/quite/long/path/name.txt");
		mu_check(strlen(buf) < cap);
		mu_check(buf[cap - 1] == '\0' || buf[cap - 1] == 'X');
	}

	/* cap 0 writes nothing at all. */
	memset(buf, 'X', sizeof(buf));
	progress_copy_path(buf, 0, "abcdef");
	mu_check(buf[0] == 'X');
}

/*
 * A path is shortened twice on its way to the screen: once into the worker
 * slot's fixed buffer, then again to the terminal width. Both stages use
 * ellipsize_path(), so the drawn line carries exactly one "…" -- not one
 * marker per stage -- and still shows the real head and the real basename.
 */
MU_TEST(test_progress_path_two_stage_render) {
	char deep[9000];
	char slot[PATH_MAX + 1];
	char drawn[PATH_MAX + 4];
	size_t n = 0;
	const char *p;
	int markers = 0;

	n += snprintf(deep + n, sizeof(deep) - n, "/head-marker");
	while (n < sizeof(deep) - 300)
		n += snprintf(deep + n, sizeof(deep) - n, "/%0*d", 200, 7);
	snprintf(deep + n, sizeof(deep) - n, "/basename.txt");
	mu_check(strlen(deep) > PATH_MAX);

	progress_copy_path(slot, sizeof(slot), deep);
	ellipsize_path(slot, drawn, sizeof(drawn), 100);

	for (p = drawn; (p = strstr(p, "…")); p += strlen("…"))
		markers++;
	mu_check(markers == 1);
	mu_check(strstr(drawn, "/head-marker") == drawn);
	mu_check(strstr(drawn, "basename.txt") != NULL);
}

static gpointer pop_one(gpointer arg)
{
	struct file_to_scan **got = arg;

	*got = scan_workq_pop(&scan_workq);
	return NULL;
}

MU_TEST(test_starved_worker_line_reads_idle) {
	/*
	 * A csum worker holds its display line across files, so the status the
	 * last file left behind ("commit") is what a starved queue would keep
	 * showing - for the whole rest of a walk-bound run. The worker publishes
	 * how long it has been waiting for its next file and the renderer draws
	 * a long enough wait as idle; a short one (the gap between two small
	 * files) must still show the file's real status, or the line flickers.
	 */
	memset(&scan_workq, 0, sizeof(scan_workq));

	struct file_to_scan file = { .filesize = 4096, .file_position = 1 };
	struct file_to_scan *got = NULL;
	struct pscan_thread *slot = pscan_claim_slot(4242, thread_committing);

	/* Waiting on an empty queue: the worker blocks in the pop. */
	GThread *popper = g_thread_new("pop", pop_one, &got);

	pscan_slot_waiting(slot, true);
	mu_check(!slot_is_idle(slot));			/* just started waiting */
	slot->waiting_since -= IDLE_AFTER_US + 1;	/* ... a while ago */
	mu_check(slot_is_idle(slot));

	/* Still claimed while it waits, so no sibling takes over its line. */
	mu_check(pscan_claim_slot(4243, thread_scanning) != slot);

	/* Back to work: the line shows the file again, not idle. */
	scan_workq_push(&file);
	g_thread_join(popper);
	pscan_slot_waiting(slot, false);
	mu_check(got == &file);
	mu_check(!slot_is_idle(slot));

	pscan_free_threads();
	memset(&scan_workq, 0, sizeof(scan_workq));
}

/* Within eps of expected. */
MU_TEST(test_scan_eta) {
	const uint64_t GiB = 1ull << 30, W = 1ull << 30;   /* 1 GiB per-file weight */

	/* Weighted progress: work = bytes + W*files, ETA = elapsed*(total-done)/done. */

	/* Nothing scanned yet -> no estimate. */
	mu_check(scan_eta_seconds(0, 0, 4 * GiB, 0, W, 10.0) < 0.0);

	/* Pure files (weight is what counts): 1 of 4 files done in 10 s -> 30 s left.
	 * done_work = W, total_work = 4*W, eta = 10*(4-1)/1. */
	mu_check(near(scan_eta_seconds(0, 1, 0, 4, W, 10.0), 30.0, 1e-6));

	/* Pure bytes: 2 of 8 GiB in 12 s -> 36 s. */
	mu_check(near(scan_eta_seconds(2 * GiB, 0, 8 * GiB, 0, W, 12.0), 36.0, 1e-6));

	/* Mixed, weight ties them together: done_work = 1 GiB + W*1 = 2 GiB,
	 * total_work = 1 GiB + W*5 = 6 GiB, eta = 10*(6-2)/2 = 20 s. */
	mu_check(near(scan_eta_seconds(GiB, 1, GiB, 5, W, 10.0), 20.0, 1e-6));

	/* A larger weight up-weights the remaining files, raising the estimate:
	 * done_work = 1 GiB + 2 GiB*1 = 3 GiB, total = 1 GiB + 2 GiB*5 = 11 GiB,
	 * eta = 10*(11-3)/3. */
	mu_check(near(scan_eta_seconds(GiB, 1, GiB, 5, 2 * GiB, 10.0),
		      10.0 * 8.0 / 3.0, 1e-6));

	/* Done >= total -> 0, never negative or a fallback signal. */
	mu_check(near(scan_eta_seconds(4 * GiB, 4, 4 * GiB, 4, W, 10.0), 0.0, 1e-6));
}
