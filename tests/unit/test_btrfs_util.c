/*
 * The btrfs ioctl wrappers, asked what they do when the ioctl says no.
 *
 * Compiled as part of tu_plain.c: these are public, so nothing here needs a
 * static.
 *
 * All 28 of this file's mutants used to survive, and the triage said the only
 * honest move was a fault-injection seam. It turns out no seam is needed for
 * half of it: every one of these takes an `fd`, so handing it a descriptor
 * that is not a btrfs subvolume makes the kernel refuse and the error path
 * runs. That is not a stub or an injected failure - it is the real ioctl
 * returning the real errno a non-btrfs filesystem returns, which is the case
 * oans meets whenever someone points it at ext4.
 *
 * The success paths still need btrfs and stay uncovered here; CI runs the
 * end-to-end suite on it.
 */

/*
 * A descriptor that is definitely not a btrfs subvolume.
 *
 * It has to be a filesystem that cannot be btrfs, not merely one that usually
 * isn't: this opened "/" until a Fedora dev box - btrfs root, which is the
 * default there and the shape oans exists for - answered SUBVOL_INFO for real
 * and failed the test. CI never saw it, because CI's root is not btrfs.
 *
 * procfs can never be btrfs and is always mounted on the only platform this
 * builds for, so the ioctl reaches a filesystem that has no handler for it and
 * refuses with ENOTTY - the same real error a user pointing oans at ext4 gets.
 */
static int not_a_subvol(void)
{
	int fd = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		abort();
	return fd;
}

MU_TEST(test_a_refused_ioctl_is_an_error_not_a_zero_answer) {
	int fd = not_a_subvol();
	uint64_t subvol = 12345;
	uuid_t uuid;
	bool rdonly = true;

	/*
	 * Each must *report*. Answering 0 while leaving the out-parameter
	 * untouched is the failure that matters: a caller then treats
	 * uninitialised stack as a subvolume id, a filesystem uuid, or a
	 * read-only flag - and #182 turns on precisely that last one.
	 */
	mu_assert(lookup_btrfs_subvol(fd, &subvol) != 0,
		  "a refused SUBVOL_INFO reported success");
	mu_assert(btrfs_get_fsuuid(fd, &uuid) != 0,
		  "a refused FS_INFO reported success");
	mu_assert(btrfs_subvol_is_readonly(fd, &rdonly) != 0,
		  "a refused GET_SUBVOL_INFO reported success");

	close(fd);
}

MU_TEST(test_a_closed_descriptor_is_refused_too) {
	int fd = not_a_subvol();
	uint64_t subvol;

	close(fd);
	/*
	 * EBADF rather than ENOTTY, so this is a different errno down the same
	 * branch - which is what says the wrapper checks the return value and
	 * not one particular error.
	 */
	mu_assert(lookup_btrfs_subvol(fd, &subvol) != 0,
		  "an ioctl on a closed descriptor reported success");
}
