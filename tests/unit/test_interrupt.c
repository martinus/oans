/*
 * SIGINT and SIGTERM flushing the open batch (#201).
 *
 * Its own translation unit. interrupt.c is #included rather than linked: the
 * flag, the "reported" latch and the test hooks are statics in it, and
 * resetting them between tests is how this suite drives a signal without
 * raising one.
 */

static struct sigaction intr_disposition(int signo)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	if (sigaction(signo, NULL, &sa))
		abort();
	return sa;
}

/* Back to a clean slate between scenarios. */
static void intr_reset(void)
{
	atomic_store(&caught_signal, 0);
	atomic_store(&reported, false);
	atomic_store(&tick_files, 0);
	tick_batches = 0;
	limit_files = 0;
	limit_batches = 0;
	test_signal = SIGINT;
	unsetenv("DUPEREMOVE_INTERRUPT_AFTER");
	unsetenv("DUPEREMOVE_INTERRUPT_AFTER_BATCHES");
	unsetenv("DUPEREMOVE_INTERRUPT_SIGNAL");
	/*
	 * And the dispositions, which nothing else here would restore: leaving
	 * the handler installed means a SIGTERM aimed at a hung ./test is
	 * swallowed once and appears to do nothing.
	 */
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
}

MU_TEST(test_the_interrupt_flag_and_its_test_hooks) {
	struct sigaction sa;

	intr_reset();
	mu_check(!interrupted());
	mu_assert(interrupt_signo() == 0, "a signal was recorded before any arrived");

	/*
	 * Both flags matter and neither is decoration. SA_RESTART keeps a
	 * walker blocked in read() from failing with EINTR, so no error path
	 * has to learn about signals; SA_RESETHAND is what makes the second
	 * Ctrl-C kill at once, which is what a user hammering it expects.
	 * Both signals get the same treatment - `systemctl stop` sends TERM.
	 */
	interrupt_install();
	for (int signo = 0; signo < 2; signo++) {
		int s = signo ? SIGTERM : SIGINT;

		sa = intr_disposition(s);
		mu_check(sa.sa_handler == interrupt_handler);
		mu_check((sa.sa_flags & SA_RESTART) != 0);
		mu_check((sa.sa_flags & SA_RESETHAND) != 0);
	}

	/*
	 * The file hook raises at exactly the Nth tick. Counting files that
	 * have *finished hashing* rather than queued ones is what makes the
	 * durability assertion in the integration suite exact, so the count
	 * being off by one is not cosmetic.
	 */
	intr_reset();
	setenv("DUPEREMOVE_INTERRUPT_AFTER", "3", 1);
	interrupt_install();			/* re-reads the hooks */
	mu_assert(limit_files == 3, "the file limit was not read from the environment");

	interrupt_test_file_tick();
	interrupt_test_file_tick();
	mu_assert(!interrupted(), "the hook raised before its limit");
	interrupt_test_file_tick();
	mu_assert(interrupted(), "the hook did not raise at its limit");
	mu_check(interrupt_signo() == SIGINT);

	/* SA_RESETHAND really did put the default back, which is why the
	 * second signal kills rather than running the handler again. */
	sa = intr_disposition(SIGINT);
	mu_assert(sa.sa_handler == SIG_DFL,
		  "the handler was not reset on delivery");

	/*
	 * Once the flag is up the hook stops counting entirely, which is the
	 * other half of `!limit_files || interrupted()`. On the file side that
	 * cannot raise a second time - the comparison is `==` against a counter
	 * that only grows - so what the guard saves is an atomic increment per
	 * file after shutdown has begun. It is the *batch* hook, comparing with
	 * `>=`, where losing this guard raises again and kills.
	 */
	{
		unsigned long at_raise = atomic_load(&tick_files);

		mu_check(interrupted());	/* still set, deliberately */
		interrupt_test_file_tick();
		interrupt_test_file_tick();
		mu_assert(atomic_load(&tick_files) == at_raise,
			  "the hook kept counting after the run was interrupted");
	}

	/*
	 * And now the reason the comparison is `==` rather than `>=`: with the
	 * counter already past the limit, further ticks must not raise. Under
	 * `>=` every later tick would, and with the disposition back to the
	 * default the second one kills the process - the very thing this hook
	 * exists to test against. The `interrupted()` early return is not what
	 * saves it here, because the flag is cleared first.
	 */
	atomic_store(&caught_signal, 0);
	interrupt_install();			/* re-arm, so a raise would not kill */
	for (int i = 0; i < 4; i++)
		interrupt_test_file_tick();
	mu_assert(!interrupted(), "a tick past the limit raised again");

	/*
	 * The batch hook counts generation batches instead, and TERM selects
	 * the other signal - `systemctl stop oans@...` is the scheduled case
	 * this whole feature was written for.
	 */
	intr_reset();
	setenv("DUPEREMOVE_INTERRUPT_AFTER_BATCHES", "2", 1);
	setenv("DUPEREMOVE_INTERRUPT_SIGNAL", "TERM", 1);
	interrupt_install();
	mu_assert(limit_batches == 2, "the batch limit was not read");
	mu_assert(test_signal == SIGTERM, "TERM did not select SIGTERM");

	interrupt_test_batch_tick();
	mu_assert(!interrupted(), "the batch hook raised before its limit");
	interrupt_test_batch_tick();
	mu_assert(interrupted(), "the batch hook did not raise at its limit");
	mu_assert(interrupt_signo() == SIGTERM,
		  "the run was stopped by the wrong signal");

	/* Unset means off: a hook that ticked without a limit would raise on
	 * the first file of every ordinary run. */
	intr_reset();
	interrupt_install();
	/*
	 * Bounded by !interrupted(), which is not caution about the test but
	 * about the suite: the batch hook compares with >=, so deleting its
	 * `!limit_batches || interrupted()` guard makes 1 >= 0 true on every
	 * call. The first raise is handled and SA_RESETHAND puts the default
	 * back; a second would terminate the process, and every test after
	 * this one would simply never run. Stopping at the first raise turns
	 * that mutant into a clean failure below.
	 */
	for (int i = 0; i < 8 && !interrupted(); i++) {
		interrupt_test_file_tick();
		interrupt_test_batch_tick();
	}
	mu_assert(!interrupted(), "a hook fired with no limit set");

	intr_reset();
}

/*
 * The wind-down notice is said once, and only once there is something to say.
 *
 * Every loop that notices the flag may call this, and several do - the point
 * of it is that the pause between the signal and the exit does not look like a
 * hang, and a line repeated once per worker would look like something worse.
 * The `reported` latch is function-local, so what is observable is the output
 * itself; eprintf() reaches stderr, which is capturable.
 */
MU_TEST(test_the_wind_down_notice_is_said_once) {
	char path[] = "/tmp/oans-intr-XXXXXX";
	int fd = mkstemp(path);
	int saved = dup(STDERR_FILENO);
	char buf[4096] = {0};
	unsigned int seen = 0;
	off_t quiet_before;
	ssize_t n;

	if (fd < 0 || saved < 0)
		abort();

	intr_reset();
	dup2(fd, STDERR_FILENO);

	/* Nothing has happened yet, so there is nothing to announce. The
	 * offset is read here and judged after stderr is back, so no assertion
	 * fires while the capture is up. */
	interrupt_report();
	fflush(stderr);
	quiet_before = lseek(fd, 0, SEEK_END);

	/* Now there is - and saying it twice would be the bug. */
	atomic_store(&caught_signal, SIGINT);
	interrupt_report();
	interrupt_report();
	fflush(stderr);

	dup2(saved, STDERR_FILENO);
	close(saved);

	mu_assert(quiet_before == 0,
		  "the notice was printed before any signal arrived");

	n = pread(fd, buf, sizeof(buf) - 1, 0);
	close(fd);
	unlink(path);
	if (n < 0)
		abort();
	buf[n] = '\0';

	for (const char *p = buf; (p = strstr(p, "Interrupted by")); p++)
		seen++;
	mu_assert(seen == 1, "the wind-down notice was not said exactly once");
	/* And it names the signal, since SIGTERM is the scheduled-stop case. */
	mu_check(strstr(buf, "SIGINT") != NULL);

	intr_reset();
}

/*
 * ---------------------------------------------------------------------------
 * fiemap against a real file
 *
 * The pure half of fiemap.c is already well covered - fiemap_maps_share() is
 * at 87% killed, phys_set_merge() 84% - because synthetic records beat
 * coaxing a filesystem into a layout. What was at 0% is everything that takes
 * a *file descriptor*: every function here that issues FS_IOC_FIEMAP.
 *
 * Those need no reflink filesystem, only FIEMAP - but not every filesystem
 * has it. tmpfs installs no ->fiemap at all, so the ioctl fails with
 * EOPNOTSUPP before the filesystem is consulted, and CLAUDE.md notes that /tmp
 * is tmpfs on the usual dev box. So fm_open() asks first and says so when the
 * answer is no, rather than failing an assertion that has nothing to do with
 * the code under test. DUPEREMOVE_TEST_DIR is preferred when set, since CI
 * guarantees btrfs or XFS there.
 *
 * What these must not do is assert a particular extent count: how a
 * filesystem lays out a write is its business. Everything below is an
 * invariant that holds whatever the layout turns out to be.
 * ---------------------------------------------------------------------------
 */
