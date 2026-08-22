#ifndef	__PROPTEST_H__
#define	__PROPTEST_H__

/*
 * proptest.h - property-based testing for the C unit suite.
 *
 * An ordinary test names an input and the answer it expects. A property names
 * a *relationship* that has to hold for every input, and then goes looking:
 *
 *	MU_TEST(test_sanitize_ctrl_is_total) {
 *		declare_prop(p, 20000);
 *
 *		while (prop_next(&p)) {
 *			char in[32], out[128];
 *
 *			gen_hostile_name(&p, in, sizeof(in));
 *			sanitize_ctrl(in, out, sizeof(out));
 *			prop_check(&p, !has_ctrl(out));
 *		}
 *	}
 *
 * Generators live with the properties that use them rather than here: what
 * makes an input interesting is a fact about the function under test, not
 * about random numbers. `prop_bytes` and the draws below are the raw material.
 *
 * The two kinds catch different things and neither replaces the other. A table
 * of cases says what the function is *for*, and reads as documentation; a
 * property says what must never happen, and reaches inputs nobody would sit
 * down and write - a 0xc2 as the last byte before the NUL, a buffer that runs
 * out one byte into an escape, two fiemap records that abut at a block
 * boundary. Every one of those is a real edge in this tree.
 *
 * Three decisions worth knowing before writing one:
 *
 * **The seed is fixed, and that is deliberate.** A test that generates fresh
 * inputs on every run is a test that fails on somebody else's commit, and a
 * suite CI cannot trust is worse than a smaller suite it can. So `make test`
 * runs the same cases every time, and finding new ones is something you ask
 * for: `OANS_PROPTEST_SEED=12345 ./test`, or `OANS_PROPTEST_SEED=random`,
 * which prints the seed it chose so the failure can be replayed. A failure
 * always names the seed and the case number - a property that cannot tell you
 * which input broke it is a worse test than the table it replaced.
 *
 * **Each property draws from its own stream**, mixed from the seed and the
 * name of the test function. Otherwise every property shares one sequence,
 * and adding a property - or reordering two - renumbers every case in the
 * ones after it. Findings would then evaporate on the next commit, which is
 * how a suite stops being worth re-running.
 *
 * **There is no shrinking.** Real property-testing libraries reduce a
 * counterexample to a minimal one, and that machinery is most of what they
 * are. The substitute here is generators that only ever produce *small*
 * inputs - strings of tens of bytes, maps of a handful of records - so a
 * counterexample is already small enough to print whole and read. Where a
 * property needs a large input to mean anything, write it as an ordinary test
 * with a fixture instead; a 4 KiB counterexample nobody can read is not a
 * finding, it is a puzzle.
 *
 * Nothing here allocates and nothing here is thread-safe: a `struct prop` is a
 * local in the test function that owns it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "minunit.h"

struct prop {
	uint64_t	state;		/* the generator, mixed per property */
	unsigned int	iteration;	/* 1-based, and what a failure names */
	unsigned int	iterations;
};

/*
 * Deliberately not carrying the property's name or the run's seed: `prop_check`
 * builds its message from `__func__` and `prop_seed()`, so a copy here would be
 * a second spelling of both - and a field that reads as state without being any
 * is what the next person adding to that message would reach for first.
 */

/*
 * splitmix64. Chosen for being eight lines rather than for its statistics: the
 * inputs below are bytes, small lengths and block-aligned offsets, and nothing
 * here is sensitive to the fine structure of the stream. Written out rather
 * than calling rand(), which is one global sequence shared by every test in the
 * process - two properties would then interleave, and neither could be replayed
 * from its own seed.
 */
static inline uint64_t prop_u64(struct prop *p)
{
	uint64_t z = (p->state += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

/* Uniform in [0, n). Modulo, whose bias is one part in 2^64/n - which for the
 * handful-of-choices draws here is not a thing any test could observe. */
static inline uint64_t prop_below(struct prop *p, uint64_t n)
{
	return n ? prop_u64(p) % n : 0;
}

static inline uint64_t prop_range(struct prop *p, uint64_t lo, uint64_t hi)
{
	return lo + prop_below(p, hi - lo + 1);
}

static inline bool prop_bool(struct prop *p)
{
	return (prop_u64(p) & 1) != 0;
}

/* One in `n` - for the rare shapes a uniform draw would almost never reach. */
static inline bool prop_chance(struct prop *p, uint64_t n)
{
	return prop_below(p, n) == 0;
}

/* Fill with uniform bytes. Callers wanting a C string terminate it themselves;
 * that is left explicit because a generator that always terminated could never
 * produce the unterminated case, which is one of the interesting ones. */
static inline void prop_bytes(struct prop *p, void *buf, size_t len)
{
	unsigned char *out = buf;

	for (size_t i = 0; i < len; i++)
		out[i] = (unsigned char)prop_u64(p);
}

/*
 * The seed for this run, and the note that says so.
 *
 * Read once per process: getenv() on every property would be the same answer
 * every time, and "random" must not mean a different seed for each of them -
 * a run has one seed or it has none that can be replayed.
 */
static inline uint64_t prop_seed(void)
{
	static uint64_t seed;
	static bool resolved;
	const char *env;

	if (resolved)
		return seed;
	resolved = true;
	seed = 0x0a2b5eed;			/* the default: reproducible */
	env = getenv("OANS_PROPTEST_SEED");
	if (env && !strcmp(env, "random")) {
		/* Only the address of a stack object and the time, because
		 * this is a test knob and not a source of randomness anyone
		 * relies on. Printed, so whatever it lands on can be replayed. */
		seed = (uint64_t)(uintptr_t)&seed ^ ((uint64_t)time(NULL) << 20);
		printf("\n[proptest] OANS_PROPTEST_SEED=%llu\n",
		       (unsigned long long)seed);
	} else if (env) {
		seed = strtoull(env, NULL, 0);
	}
	return seed;
}

/*
 * Whether to skip properties whose inputs drive PCRE2's JIT.
 *
 * Not a knob for making a failure go away: it exists because valgrind cannot
 * see into JIT-generated code. GLib compiles every --exclude pattern with
 * G_REGEX_OPTIMIZE, PCRE2 then emits machine code onto the heap, and memcheck
 * reports a "conditional jump depends on uninitialised value" for each branch
 * it cannot account for - hundreds of them, from frames with no symbol and a
 * stack address where a return address should be.
 *
 * Verified to be the library and not oans: a standalone program containing no
 * oans code, compiling 3000 generated patterns and matching 8 paths against
 * each, is *clean* under memcheck with plain `g_regex_new` and produces the
 * identical error signature the moment G_REGEX_OPTIMIZE is added. The suite's
 * own glob properties compile thousands of patterns where the fixed tests
 * compile a handful, which is why this only appeared with them.
 *
 * Suppressing it was the obvious alternative and is worse: the frames carry no
 * object name, so valgrind's own generated suppression is `Memcheck:Cond` over
 * `obj:*` - which would hide every uninitialised-value error in the process.
 * That check is not decoration here; it is what caught the missing memset in
 * start_running_checksum(). Losing three properties under one tool is the far
 * smaller loss, and the suite says out loud when it has done so.
 */
static inline bool prop_skip_jit(const char *name)
{
	if (!getenv("OANS_PROPTEST_NO_JIT"))
		return false;
	printf("\n[proptest] skipping %s: PCRE2's JIT is opaque to valgrind\n", name);
	return true;
}

/* Mix the property's name into its seed, so its stream is its own. */
static inline uint64_t prop_stream(const char *name)
{
	uint64_t h = prop_seed() ^ 0xcbf29ce484222325ULL;

	for (const char *c = name; *c; c++) {
		h ^= (unsigned char)*c;
		h *= 0x100000001b3ULL;
	}
	return h;
}

static inline struct prop prop_init(const char *name, unsigned int iterations)
{
	struct prop p = {
		.state = prop_stream(name),
		.iteration = 0,
		.iterations = iterations,
	};

	return p;
}

/*
 * A declaration macro rather than a statement, in the shape of util.h's
 * `declare_display_path`: `__func__` has to be read where the test function is,
 * and a `struct prop` is cheap enough to live on that frame.
 */
#define declare_prop(var, count)					\
	struct prop var = prop_init(__func__, (count))

static inline bool prop_next(struct prop *p)
{
	return p->iteration++ < p->iterations;
}

/*
 * `mu_check`, plus the two facts that make a counterexample reproducible.
 *
 * Deliberately silent on success where mu_check prints a dot: a property runs
 * thousands of cases, and thousands of dots per test would bury the suite's
 * own output - the existing 31 tests already print nearly six thousand. The
 * assertion still counts, so the tally at the end is honest about how much was
 * checked.
 *
 * `return` on failure is minunit's own convention (see mu_check), so the loop
 * stops at the first counterexample rather than reporting the same broken
 * property once per remaining case.
 */
#define prop_check(p, test) MU__SAFE_BLOCK(				\
	minunit_assert++;						\
	if (!(test)) {							\
		(void)snprintf(minunit_last_message, MINUNIT_MESSAGE_LEN,\
			"%s failed:\n\t%s:%d: %s"			\
			"\n\treplay: OANS_PROPTEST_SEED=%llu"		\
			" (case %u of %u)",				\
			__func__, __FILE__, __LINE__, #test,		\
			(unsigned long long)prop_seed(),		\
			(p)->iteration, (p)->iterations);		\
		minunit_status = 1;					\
		return;							\
	}								\
)

#endif	/* __PROPTEST_H__ */
