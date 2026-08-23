/*
 * Escaping, sizes, durations and digit grouping.
 *
 * Its own translation unit. Nothing here needs a static function, so this
 * links against the sources rather than including them.
 */

struct fenced { char before[8]; char out[32]; char after[8]; };

#define FENCE_BYTE '#'

static bool fence_intact(const struct fenced *f)
{
	for (size_t i = 0; i < sizeof(f->before); i++)
		if (f->before[i] != FENCE_BYTE || f->after[i] != FENCE_BYTE)
			return false;
	return true;
}

/* True if `f->out` is the prefix of `full` that `sz` bytes can hold. */
static bool wrote_prefix_within(const struct fenced *f, size_t sz, const char *full)
{
	if (sz == 0)
		return f->out[0] == FENCE_BYTE;	/* not even a NUL */
	if (strlen(f->out) >= sz || strncmp(f->out, full, strlen(f->out)))
		return false;
	/* Truncated only when it had to be. */
	return strlen(full) >= sz || !strcmp(f->out, full);
}

/* One fiemap record: {logical, physical, length, flags}. */
MU_TEST(test_sanitize_ctrl) {
	char out[64];

	/* Plain ASCII and legitimate multi-byte UTF-8 pass through unchanged. */
	sanitize_ctrl("plain.txt", out, sizeof(out));
	mu_check(strcmp(out, "plain.txt") == 0);
	sanitize_ctrl("café-Β.txt", out, sizeof(out));   /* é=C3A9, Β=CE92 */
	mu_check(strcmp(out, "café-Β.txt") == 0);

	/* Whitespace controls keep their familiar spelling... */
	sanitize_ctrl("a\tb\nc\rd", out, sizeof(out));
	mu_check(strcmp(out, "a\\tb\\nc\\rd") == 0);

	/* ... every other C0 control, and DEL, is named by its byte. */
	sanitize_ctrl("esc\x1b[2Jx\x07\x7f", out, sizeof(out));
	mu_check(strcmp(out, "esc\\x1b[2Jx\\x07\\x7f") == 0);

	/* C1 control U+009F (UTF-8 C2 9F): named by its code point, not by
	 * either of the two bytes that spell it (#353). */
	sanitize_ctrl("Te\xc2\x9ft", out, sizeof(out));
	mu_check(strcmp(out, "Te\\x9ft") == 0);

	/* Truncation stays NUL-terminated and within bounds, and never splits an
	 * escape: "ab" + a 4-byte escape does not fit in 6, so it stops at "ab". */
	char small[4];
	sanitize_ctrl("abcdef", small, sizeof(small));
	mu_check(strcmp(small, "abc") == 0);
	char six[6];
	sanitize_ctrl("ab\x1b" "cd", six, sizeof(six));
	mu_check(strcmp(six, "ab") == 0);

	/* ctrl_seq_len() is the one classifier; has_ctrl() the fast path out. */
	unsigned char cp = 0;

	mu_check(ctrl_seq_len((const unsigned char *)"a", &cp) == 0);
	mu_check(ctrl_seq_len((const unsigned char *)"\x1b", &cp) == 1 && cp == 0x1b);
	mu_check(ctrl_seq_len((const unsigned char *)"\x7f", &cp) == 1 && cp == 0x7f);
	/*
	 * A C1 costs two input bytes and is named by its code point - at *both*
	 * ends of the range and not just the top. Only U+009F was checked here
	 * before, which left `>= 0x80` and `<= 0x9f` each satisfied by one
	 * example: a sweep turned `>= 0x80` into `> 0x80` and `<= 0x9f` into
	 * `== 0x9f` and nothing went red, so U+0080 was reaching the terminal.
	 */
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\x80", &cp) == 2 && cp == 0x80);
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\x8f", &cp) == 2 && cp == 0x8f);
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\x9f", &cp) == 2 && cp == 0x9f);
	/* One below and one above the range, which are ordinary UTF-8. */
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\x7f", &cp) == 0);
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\xa0", &cp) == 0);
	/* 0xc2 not followed by a continuation byte is ordinary UTF-8 lead. */
	mu_check(ctrl_seq_len((const unsigned char *)"\xc2\xa9", &cp) == 0);
	/* And the whole range survives the escaper, not only its endpoints. */
	for (unsigned int c1 = 0x80; c1 <= 0x9f; c1++) {
		char name[8], want[8];

		snprintf(name, sizeof(name), "x\xc2%c", (char)c1);
		snprintf(want, sizeof(want), "x\\x%02x", c1);
		sanitize_ctrl(name, out, sizeof(out));
		mu_check(!strcmp(out, want));
	}

	mu_check(!has_ctrl("plain.txt"));
	mu_check(!has_ctrl("café-Β.txt"));
	mu_check(!has_ctrl(""));
	mu_check(has_ctrl("a\rb"));
	mu_check(has_ctrl("Te\xc2\x9ft"));

	/* path_for_display() always has room for the whole escaped path. */
	char *dup = path_for_display("a\x1b" "b\x7f");
	mu_check(dup && strcmp(dup, "a\\x1bb\\x7f") == 0);
	free(dup);
	dup = path_for_display("");
	mu_check(dup && strcmp(dup, "") == 0);
	free(dup);
	dup = path_for_display("café-Β.txt");	/* the fast path: unchanged */
	mu_check(dup && strcmp(dup, "café-Β.txt") == 0);
	free(dup);

	/*
	 * A name that is *entirely* control bytes, where the allocation is
	 * exactly SANITIZE_CTRL_MAX per byte plus the NUL and there is no slack
	 * to absorb an arithmetic slip. A mixed name has room to spare, so the
	 * cases above pass with the `+ 1` removed - and what ships then is a
	 * path silently one character short, which names a different file.
	 */
	dup = path_for_display("\x1b\x1b\x1b");
	mu_check(dup && strcmp(dup, "\\x1b\\x1b\\x1b") == 0);
	free(dup);
}

MU_TEST(test_group_u64) {
	char b[28];

	/* Under 1000: unchanged. */
	group_u64_snprintf(0, b, sizeof(b));
	mu_check(strcmp(b, "0") == 0);
	group_u64_snprintf(7, b, sizeof(b));
	mu_check(strcmp(b, "7") == 0);
	group_u64_snprintf(999, b, sizeof(b));
	mu_check(strcmp(b, "999") == 0);

	/* Separators every three digits from the right. */
	group_u64_snprintf(1000, b, sizeof(b));
	mu_check(strcmp(b, "1,000") == 0);
	group_u64_snprintf(12000, b, sizeof(b));
	mu_check(strcmp(b, "12,000") == 0);
	group_u64_snprintf(2505166, b, sizeof(b));
	mu_check(strcmp(b, "2,505,166") == 0);

	/* Full width UINT64_MAX still fits the 28-byte buffer. */
	group_u64_snprintf(18446744073709551615ull, b, sizeof(b));
	mu_check(strcmp(b, "18,446,744,073,709,551,615") == 0);

	/*
	 * Truncation, asserted on the bytes rather than on the length. `strlen
	 * <= 3` is satisfied by almost any arithmetic slip in the three
	 * `str_bytes - 1` bounds, which is why sixteen mutants across those
	 * three lines survived it.
	 */
	char small[4];
	group_u64_snprintf(2505166, small, sizeof(small));
	mu_check(!strcmp(small, "2,5"));		/* "2,505,166" cut to fit */
	char one[2];
	group_u64_snprintf(2505166, one, sizeof(one));
	mu_check(!strcmp(one, "2"));
	char just_a_nul[1];
	group_u64_snprintf(2505166, just_a_nul, sizeof(just_a_nul));
	mu_check(just_a_nul[0] == '\0');
	/* The return value is what was written, not what was wanted. */
	mu_check(group_u64_snprintf(2505166, small, sizeof(small)) == 3);
	mu_check(group_u64_snprintf(999, b, sizeof(b)) == 3);
}

/*
 * Whatever the number and whatever the room, the result is a NUL-terminated
 * prefix of the full grouping, the return value is its length, and nothing is
 * written past the buffer. The three `str_bytes - 1` bounds are the whole of
 * this function's difficulty and a table cannot walk every cut point.
 */
MU_TEST(test_prop_group_u64_truncates_to_a_prefix) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		struct fenced fenced;
		char full[32];
		/* Log-uniform, so every digit width comes up. */
		uint64_t n = prop_u64(&p) >> prop_below(&p, 64);
		size_t sz = (size_t)prop_below(&p, sizeof(fenced.out) + 1);
		int ret;

		group_u64_snprintf(n, full, sizeof(full));
		memset(&fenced, FENCE_BYTE, sizeof(fenced));
		ret = group_u64_snprintf(n, fenced.out, sz);

		prop_check(&p, fence_intact(&fenced));
		prop_check(&p, wrote_prefix_within(&fenced, sz, full));
		/* And the one part that is this function's own: it reports what it
		 * wrote, not what it wanted to. */
		prop_check(&p, (size_t)ret == (sz ? strlen(fenced.out) : 0));
	}
}

/*
 * longpath: reach a file whose absolute path exceeds PATH_MAX. Builds a chain
 * of 255-char directories (via incremental chdir, since the leaf's own path is
 * too long to pass to a syscall) under a /tmp temp dir, then checks that
 * longpath_open/longpath_stat reach the deep leaf that a plain open/stat could
 * not. Runs on tmpfs (no reflink needed); best-effort teardown climbs back out.
 */
static uint64_t size_of(const char *s)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%s", s);
	return parse_size(buf);
}

MU_TEST(test_parse_size) {
	/* Bare numbers are bytes. */
	mu_check(size_of("0") == 0);
	mu_check(size_of("1") == 1);
	mu_check(size_of("4096") == 4096);

	/* The ladder, one rung at a time. Every one of these is a separate
	 * fallthrough, and the compiler will not miss one for you. */
	mu_check(size_of("1b") == 1);
	mu_check(size_of("1k") == 1024ULL);
	mu_check(size_of("1m") == 1024ULL * 1024);
	mu_check(size_of("1g") == 1024ULL * 1024 * 1024);
	mu_check(size_of("1t") == 1024ULL * 1024 * 1024 * 1024);
	mu_check(size_of("1p") == 1024ULL * 1024 * 1024 * 1024 * 1024);
	mu_check(size_of("1e") == 1024ULL * 1024 * 1024 * 1024 * 1024 * 1024);

	/* Case is not significant. The switch spells some rungs twice and
	 * leans on tolower() for the rest, so this is not free. */
	mu_check(size_of("2K") == size_of("2k"));
	mu_check(size_of("2M") == size_of("2m"));
	mu_check(size_of("2G") == size_of("2g"));
	mu_check(size_of("2T") == size_of("2t"));
	mu_check(size_of("2P") == size_of("2p"));
	mu_check(size_of("2E") == size_of("2e"));

	/* The multiplier applies to the whole number, not the first digit. */
	mu_check(size_of("123k") == 123ULL * 1024);
	mu_check(size_of("1024k") == size_of("1m"));

	/*
	 * The error paths - an empty value, an unknown descriptor, a suffix
	 * longer than one character - are not exercised here: parse_size()
	 * calls exit() on each, which would take the whole suite with it.
	 * They are covered end-to-end in tests/integration/test_min_filesize.py.
	 */
}

/*
 * Every rung of that ladder, against the arithmetic it stands for. The table
 * above names the rungs; this says the multiplier is exactly 1024 per rung for
 * any value, which is what a mutated `*= 1024` breaks in a way one example
 * might happen to miss.
 */
MU_TEST(test_prop_parse_size_scales_by_the_suffix) {
	declare_prop(p, 20000);
	static const char rungs[] = "bkmgtpe";

	while (prop_next(&p)) {
		unsigned int level = (unsigned int)prop_below(&p, sizeof(rungs) - 1);
		/* Bounded so that the largest rung cannot overflow: 1024^6 is
		 * 2^60, leaving four bits of headroom. */
		uint64_t n = prop_below(&p, 16);
		uint64_t expect = n;
		char buf[32];

		for (unsigned int i = 0; i < level; i++)
			expect *= 1024;

		snprintf(buf, sizeof(buf), "%" PRIu64 "%c", n, rungs[level]);
		prop_check(&p, parse_size(buf) == expect);

		snprintf(buf, sizeof(buf), "%" PRIu64 "%c", n,
			 (char)toupper(rungs[level]));
		prop_check(&p, parse_size(buf) == expect);
	}
}

MU_TEST(test_human_size) {
	char buf[32];

	/* Below a kibibyte the exact byte count is printed, with no decimal -
	 * "0.0 B" for an empty file would be worse than useless. */
	human_size_snprintf(0, buf, sizeof(buf));
	mu_check(!strcmp(buf, "0 B"));
	human_size_snprintf(1023, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1023 B"));

	/* The boundary in both directions. */
	human_size_snprintf(1024, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.0 KiB"));
	human_size_snprintf(1536, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.5 KiB"));

	human_size_snprintf(1ULL << 20, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.0 MiB"));
	human_size_snprintf(1ULL << 30, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.0 GiB"));
	human_size_snprintf(1ULL << 40, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.0 TiB"));
	human_size_snprintf(1ULL << 50, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.0 PiB"));

	/* The top rung has to stop there: the loop is bounded by the size of
	 * the units array, and one rung further reads past the end of it. */
	human_size_snprintf(1ULL << 60, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1.0 EiB"));
	human_size_snprintf(UINT64_MAX, buf, sizeof(buf));
	mu_check(!strcmp(buf, "16.0 EiB"));

	/*
	 * One byte under a mebibyte reads as "1024.0 KiB" rather than
	 * "1.0 MiB": the loop stops on the computed value, which is 1023.999,
	 * and `%.1f` rounds it up afterwards. Cosmetic, and pinned here so
	 * that it is a decision rather than a surprise - a property test
	 * looking for a mantissa below 1024.0 finds this within a few thousand
	 * cases.
	 */
	human_size_snprintf((1ULL << 20) - 1, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1024.0 KiB"));

	/* A caller with no room gets nothing written and is told so. */
	mu_check(human_size_snprintf(1024, buf, 0) == 0);
}

/*
 * Whatever the size, the unit is one of the seven the array holds and the
 * mantissa is in range for it. A mutated loop bound is an out-of-bounds read
 * of `units` - which a plain build prints as whatever followed the array.
 */
MU_TEST(test_prop_human_size_picks_a_real_unit) {
	declare_prop(p, 20000);
	static const char * const units[] = { "B", "KiB", "MiB", "GiB",
					      "TiB", "PiB", "EiB" };

	while (prop_next(&p)) {
		char buf[32], unit[8];
		double v;
		uint64_t size;
		bool known = false;

		/* Log-uniform, so every rung is reached about equally often;
		 * a uniform draw over uint64 is a byte count above a
		 * pebibyte essentially every time. */
		size = prop_u64(&p) >> prop_below(&p, 64);

		prop_check(&p, human_size_snprintf(size, buf, sizeof(buf)) > 0);
		prop_check(&p, sscanf(buf, "%lf %7s", &v, unit) == 2);
		for (unsigned int i = 0; i < ARRAY_SIZE(units); i++)
			known = known || !strcmp(unit, units[i]);
		prop_check(&p, known);
		/* Bytes are printed whole and unscaled; anything else was
		 * divided down until it was under a kibibyte.
		 *
		 * The upper bound is 1024.0 and not just below it, because the
		 * division stops on the *computed* value and `%.1f` then
		 * rounds: 1 MiB - 1 divides to 1023.999 and prints as
		 * "1024.0 KiB". Pinned as a case in test_human_size rather
		 * than tightened away - it is cosmetic, and where the unit
		 * boundary sits is not a testing commit's to move. */
		if (!strcmp(unit, "B"))
			prop_check(&p, v == (double)size && size < 1024);
		else
			prop_check(&p, v >= 1.0 && v <= 1024.0);
	}
}

MU_TEST(test_human_duration) {
	char buf[32];

	human_duration_snprintf(0, buf, sizeof(buf));
	mu_check(!strcmp(buf, "0s"));
	human_duration_snprintf(59, buf, sizeof(buf));
	mu_check(!strcmp(buf, "59s"));

	/* Seconds are zero-padded once minutes appear, so the field does not
	 * jump width as a scan runs - the progress line is redrawn in place. */
	human_duration_snprintf(60, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1m00s"));
	human_duration_snprintf(61, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1m01s"));
	human_duration_snprintf(3599, buf, sizeof(buf));
	mu_check(!strcmp(buf, "59m59s"));

	/* Past an hour the seconds are dropped, not the minutes. */
	human_duration_snprintf(3600, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1h00m"));
	human_duration_snprintf(3660, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1h01m"));
	human_duration_snprintf(86399, buf, sizeof(buf));
	mu_check(!strcmp(buf, "23h59m"));

	/* Rounded to the nearest second rather than truncated, so an ETA of
	 * 0.6s does not read as "0s" for the whole of its last second. */
	human_duration_snprintf(0.6, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1s"));
	human_duration_snprintf(59.5, buf, sizeof(buf));
	mu_check(!strcmp(buf, "1m00s"));

	mu_check(human_duration_snprintf(1, buf, 0) == 0);
}

/*
 * The rendering has to be readable *back*: whatever the duration, parsing the
 * string returns the same number of seconds, to the resolution that form
 * carries. That catches the swaps a table of examples reads straight past - a
 * `/ 60` against a `% 60`, or minutes and seconds the wrong way round, both of
 * which are right for some of the examples anyone would think to write.
 */
MU_TEST(test_prop_human_duration_reads_back) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char buf[32];
		unsigned long s = (unsigned long)prop_below(&p, 100UL * 3600);
		unsigned long h, m, sec;

		human_duration_snprintf((double)s, buf, sizeof(buf));

		if (s < 60) {
			prop_check(&p, sscanf(buf, "%lus", &sec) == 1);
			prop_check(&p, sec == s);
		} else if (s < 3600) {
			prop_check(&p, sscanf(buf, "%lum%lus", &m, &sec) == 2);
			prop_check(&p, sec < 60);
			prop_check(&p, m * 60 + sec == s);
		} else {
			prop_check(&p, sscanf(buf, "%luh%lum", &h, &m) == 2);
			prop_check(&p, m < 60);
			/* Seconds are dropped rather than rounded into the
			 * minute, so what is printed accounts for everything
			 * except them. */
			prop_check(&p, h * 3600 + m * 60 == s - s % 60);
		}
	}
}

MU_TEST(test_num_digits) {
	/* Zero has no digits by this definition, which is what its one caller
	 * wants: a column width for a counter that has not started. */
	mu_check(num_digits(0) == 0);
	mu_check(num_digits(1) == 1);
	mu_check(num_digits(9) == 1);
	mu_check(num_digits(10) == 2);
	mu_check(num_digits(99) == 2);
	mu_check(num_digits(100) == 3);
	mu_check(num_digits(ULLONG_MAX) == 20);
}

/* Against printf, which is the definition anyone actually means by it. */
MU_TEST(test_prop_num_digits_matches_printf) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char buf[32];
		/* Log-uniform: a uniform draw is a twenty-digit number
		 * essentially every time, and every other width is the
		 * interesting one. */
		unsigned long long n = prop_u64(&p) >> prop_below(&p, 64);

		snprintf(buf, sizeof(buf), "%llu", n);
		prop_check(&p, num_digits(n) == (n ? (int)strlen(buf) : 0));
	}
}

static void gen_hostile_name(struct prop *p, char *buf, size_t sz)
{
	size_t len = (size_t)prop_below(p, sz - 1);

	for (size_t i = 0; i < len; i++) {
		unsigned char c;

		switch (prop_below(p, 8)) {
		case 0:				/* a C0 control or DEL */
			c = prop_chance(p, 4) ? 0x7f
					      : (unsigned char)prop_below(p, 0x20);
			break;
		case 1:				/* the lead byte of a C1 */
			c = 0xc2;
			break;
		case 2:				/* a C1 trail, or a stray one */
			c = (unsigned char)prop_range(p, 0x80, 0x9f);
			break;
		case 3:				/* any byte at all */
			c = (unsigned char)prop_u64(p);
			break;
		default:			/* something a name is made of */
			c = (unsigned char)prop_range(p, 'a', 'z');
			break;
		}
		/* A NUL would end the string early and silently shrink every
		 * case that drew one; the interesting truncation is the
		 * buffer's, and that is generated on purpose below. */
		buf[i] = c ? (char)c : 'x';
	}
	buf[len] = '\0';
}

/*
 * The guarantee #202 rests on: whatever bytes are in a file's name, nothing
 * that reaches the terminal can still act on it. Stated over the classifier
 * rather than over a list of characters, so the two cannot drift - adding an
 * encoding to ctrl_seq_len() tightens this test in the same commit.
 */
MU_TEST(test_prop_sanitize_ctrl_leaves_nothing_dangerous) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char in[24], out[SANITIZE_CTRL_MAX * 24 + 1];

		gen_hostile_name(&p, in, sizeof(in));
		sanitize_ctrl(in, out, sizeof(out));
		prop_check(&p, !has_ctrl(out));
		/*
		 * And again in raw bytes, which is not the redundancy it looks
		 * like. `has_ctrl` is `ctrl_seq_len` and so is the escaper, so
		 * a mutation *inside the classifier* leaves the two agreeing
		 * and this property blind to it - measured: narrowing the C1
		 * test to `> 0x80` was caught by nothing here. A property
		 * phrased in terms of the function under test cannot see the
		 * function being wrong, only inconsistent.
		 */
		for (const unsigned char *q = (const unsigned char *)out; *q; q++) {
			prop_check(&p, *q >= 0x20 && *q != 0x7f);
			prop_check(&p, !(q[0] == 0xc2 && q[1] >= 0x80 && q[1] <= 0x9f));
		}
		/*
		 * And within the bound `path_for_display` sizes its allocation
		 * by, which is load-bearing rather than decorative: an encoding
		 * that expanded further would overflow a heap buffer on the
		 * first crafted name. Asserted here rather than in a property
		 * of its own, which would be this one's setup plus a line.
		 */
		prop_check(&p, strlen(out) <= SANITIZE_CTRL_MAX * strlen(in));
	}
}

/*
 * A short buffer must truncate and never split an escape - half of `\xNN` is
 * two characters of something else. Stated as "the short answer is a prefix of
 * the long one", which says that and also that nothing else changes with the
 * buffer size; a rule about the last four bytes would not.
 *
 * The canaries are the other half: this is the only caller that can run out of
 * room, so a fencepost here writes past a heap allocation in production.
 */
MU_TEST(test_prop_sanitize_ctrl_truncates_on_whole_escapes) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char in[24], full[SANITIZE_CTRL_MAX * 24 + 1];
		struct fenced fenced;
		size_t sz = (size_t)prop_below(&p, sizeof(fenced.out) + 1);

		gen_hostile_name(&p, in, sizeof(in));
		memset(&fenced, FENCE_BYTE, sizeof(fenced));
		sanitize_ctrl(in, fenced.out, sz);
		sanitize_ctrl(in, full, sizeof(full));

		prop_check(&p, fence_intact(&fenced));
		prop_check(&p, wrote_prefix_within(&fenced, sz, full));
	}
}

/*
 * Every real name takes the fast path, and it has to be exactly that - a fast
 * path that also *changed* the name would rewrite most of what oans prints.
 */
MU_TEST(test_prop_sanitize_ctrl_is_identity_on_ordinary_names) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char in[24], out[sizeof(in)];
		size_t len = (size_t)prop_below(&p, sizeof(in) - 1);

		for (size_t i = 0; i < len; i++) {
			unsigned char c;

			/* Anything ctrl_seq_len() does not flag, which
			 * includes the high bytes of ordinary UTF-8. */
			do {
				c = (unsigned char)prop_u64(&p);
			} while (!c || c < 0x20 || c == 0x7f || c == 0xc2);
			in[i] = (char)c;
		}
		in[len] = '\0';

		prop_check(&p, !has_ctrl(in));
		sanitize_ctrl(in, out, sizeof(out));
		prop_check(&p, strcmp(out, in) == 0);

		_cleanup_(freep) char *disp = path_for_display(in);

		prop_check(&p, disp && !strcmp(disp, in));
	}
}

/*
 * #159's whole design constraint: the digest a resumed hash produces is the
 * digest of the same bytes, whatever offsets it was interrupted at. The one
 * fixed case above splits at 1000 bytes because that is not a multiple of
 * XXH3's internal buffer; this asks the same question at every offset, and at
 * several of them in a row, which is what a 1 TiB file actually does.
 */
