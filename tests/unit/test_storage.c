/*
 * The io-threads heuristic and the line describing it.
 *
 * Part of the oans unit suite. tests/unit/main.c includes this file along
 * with the sources it exercises, so a test still reaches a static function
 * the way it always did.
 */

MU_TEST(test_storage_recommend_io_threads) {
	struct storage_profile p;

	/* SSD / non-rotational: keep the full CPU-capped default (cap 8). */
	p = (struct storage_profile){ .rotational = false,
		.rotational_known = true, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 4) == 4);
	mu_check(storage_recommend_io_threads(&p, 32) == 8);	/* capped */

	/* Unknown media falls back to the same default, never fewer. */
	p = (struct storage_profile){ .rotational = false,
		.rotational_known = false, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 16) == 8);
	mu_check(storage_recommend_io_threads(&p, 2) == 2);

	/* Single spinning disk: few concurrent readers (seek-bound), max 4. */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = true, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 32) == 4);
	mu_check(storage_recommend_io_threads(&p, 2) == 2);	/* fewer cores wins */

	/* HDD pool: ~2 readers per spindle, still capped at 8 and by cores. */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = true, .num_devices = 2 };
	mu_check(storage_recommend_io_threads(&p, 32) == 4);
	p.num_devices = 4;
	mu_check(storage_recommend_io_threads(&p, 32) == 8);	/* 2*4, capped 8 */
	mu_check(storage_recommend_io_threads(&p, 6) == 6);	/* cores limit */

	/* Degenerate CPU count still yields at least one thread. */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = true, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 0) == 1);

	/*
	 * A device count of zero takes the single-disk branch too, and this
	 * is the one assertion here that is about a hazard rather than a
	 * preference. storage_detect() seeds num_devices = 1, but a zeroed
	 * profile is what a caller that skipped it holds - and the pool arm
	 * would compute 2 * 0 and recommend *no* reader threads at all, which
	 * sizes three pools. `<= 1` is what keeps that unreachable; `== 1`
	 * reads identically and does not.
	 */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = true, .num_devices = 0 };
	mu_check(storage_recommend_io_threads(&p, 32) == 4);

	/*
	 * Between the clamp and the cap: with three cores the single-disk arm
	 * must yield the cores, not the constant. Every case above sits at
	 * base >= 4 or base <= 2, where `base < 4 ? base : 4` and a clamp of
	 * three agree - so the clamp could be lowered and nothing noticed.
	 */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = true, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 3) == 3);
}

MU_TEST(test_prop_a_recommendation_is_never_zero_and_never_over_the_cap) {
	declare_prop(p, 20000);

	/*
	 * The table above says what each branch should answer. This says what
	 * no branch may answer, and it is deliberately phrased without
	 * reference to the branches: at least one reader thread, and never
	 * more than the CPU cap allows.
	 *
	 * That independence is the point. The floor is the same claim the
	 * `num_devices <= 1` test carries - a zeroed profile reaching the pool
	 * arm computes 2 * 0 - but a table case asserts it at one profile,
	 * where this asserts it at every combination of the three inputs,
	 * including the ones nobody thought to write down. The result sizes
	 * three thread pools, so zero is not a wrong number, it is a hang.
	 */
	while (prop_next(&p)) {
		struct storage_profile sp;
		unsigned int ncpus = (unsigned int)prop_below(&p, 260);
		unsigned int got, ceiling;

		sp.rotational_known = prop_bool(&p);
		sp.rotational = prop_bool(&p);
		/*
		 * Log-uniform over a bounded range, so a pool of 3 and a pool
		 * of a million both come up. The bound is deliberate and is
		 * the one caveat on the claim below: `2 * p->num_devices` is
		 * unsigned int arithmetic, so a count at or above 2^31 wraps
		 * and the pool arm can answer zero by a second route the
		 * `<= 1` guard knows nothing about. num_devices is a btrfs
		 * pool member count read from BTRFS_IOC_FS_INFO, so no such
		 * filesystem exists; widening this generator would report an
		 * unreachable input as a counterexample and say nothing about
		 * the guard the property is here to hold.
		 */
		sp.num_devices = (unsigned int)
			((prop_u64(&p) >> prop_below(&p, 64)) & 0xfffff);

		got = storage_recommend_io_threads(&sp, ncpus);
		ceiling = ncpus < AUTO_THREADS_CAP ? ncpus : AUTO_THREADS_CAP;
		if (ceiling < 1)
			ceiling = 1;

		prop_check(&p, got >= 1);
		prop_check(&p, got <= ceiling);
	}
}

MU_TEST(test_storage_describe) {
	struct storage_profile p;
	char buf[64];

	/*
	 * The line a run prints for its own storage, and the only place the
	 * profile is rendered rather than acted on. Its device-count test had
	 * no test at all, so every spelling of it - `>= 1`, `> 0`, `> 2` -
	 * described a single disk as a pool or a pool as a single disk with
	 * nothing going red.
	 */
	p = (struct storage_profile){ .rotational = false,
		.rotational_known = true, .num_devices = 1 };
	storage_describe(&p, buf, sizeof(buf));
	mu_assert_string_eq("single device, non-rotational (SSD)", buf);

	/* Two is the boundary: the first count that is a pool. */
	p.num_devices = 2;
	p.rotational = true;
	storage_describe(&p, buf, sizeof(buf));
	mu_assert_string_eq("btrfs pool of 2 devices, rotational (HDD)", buf);

	/* Unknown media outranks the rotational flag, which is then stale. */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = false, .num_devices = 1 };
	storage_describe(&p, buf, sizeof(buf));
	mu_assert_string_eq("single device, unknown media", buf);

	/* Truncation is snprintf's job, but the result must stay a C string:
	 * this is what the caller passes to a %s. */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = true, .num_devices = 12 };
	memset(buf, 'x', sizeof(buf));
	storage_describe(&p, buf, 12);
	mu_assert_string_eq("btrfs pool ", buf);	/* 11 chars + NUL */
	mu_check(buf[12] == 'x');	/* nothing written past the length */
}
