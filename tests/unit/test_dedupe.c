/*
 * Classifying what the FIDEDUPERANGE probe answered (#224).
 *
 * Part of the oans unit suite. tests/unit/main.c includes this file along
 * with the sources it exercises, so a test still reaches a static function
 * the way it always did.
 */

MU_TEST(test_dedupe_classify_probe)
{
	/* Dispatched, and the destination was processed: SAME or DIFFERS. */
	mu_assert_int_eq(DEDUPE_SUPPORT_YES, dedupe_classify_probe(0, 0, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_YES, dedupe_classify_probe(0, 0, 1));
	/* errno is meaningless on success and must not be consulted. */
	mu_assert_int_eq(DEDUPE_SUPPORT_YES,
			 dedupe_classify_probe(0, EINVAL, 0));

	/* A stacking filesystem puts the lower filesystem's refusal in status
	 * and leaves rc/errno clean - the case dedupe_probe_fd documents. */
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(0, 0, -EINVAL));
	/* A status that does name the filesystem is still an answer. */
	mu_assert_int_eq(DEDUPE_SUPPORT_NO,
			 dedupe_classify_probe(0, 0, -EOPNOTSUPP));
	mu_assert_int_eq(DEDUPE_SUPPORT_NO,
			 dedupe_classify_probe(0, 0, -ENOTTY));

	/* Answers about the filesystem from the ioctl itself. EOPNOTSUPP is
	 * what ext4 returns (measured); ENOTTY is a kernel that does not know
	 * the ioctl. status is not filled in when the ioctl fails. */
	mu_assert_int_eq(DEDUPE_SUPPORT_NO,
			 dedupe_classify_probe(-1, EOPNOTSUPP, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_NO,
			 dedupe_classify_probe(-1, ENOTTY, 0));

	/*
	 * Answers about this file or this caller. EINVAL is the important one:
	 * it is documented for "the filesystem does not support deduplicating
	 * the ranges of the given files" *and* for ordinary per-file
	 * conditions, so reading it as NO would condemn a filesystem on the
	 * evidence of one awkward file.
	 */
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EINVAL, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EACCES, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EROFS, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EPERM, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EISDIR, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, EBADF, 0));
	mu_assert_int_eq(DEDUPE_SUPPORT_UNKNOWN,
			 dedupe_classify_probe(-1, ETXTBSY, 0));
}
