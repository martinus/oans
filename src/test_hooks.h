/*
 * test_hooks.h
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

#ifndef __TEST_HOOKS_H__
#define __TEST_HOOKS_H__

#include <stdlib.h>

/*
 * Test hooks (#295): the DUPEREMOVE_* environment variables the test suites use
 * to steer a run - interrupt it at a named point, fail a write, hold a worker
 * back, shrink an interval. Every one is read through test_hook_env(), which is
 * also how to find them all.
 *
 * `make TEST_HOOKS=0` builds oans without them: test_hook_env() is then NULL,
 * so none can be set from outside, and the compiler drops the names and the
 * code that only a hook reaches. The default keeps them, since the integration
 * suite needs them; it skips what it cannot run on a binary built without
 * (`oans --version` says so). The unit suite always has them - it #includes the
 * sources and never sees the flag, so the default below applies.
 *
 * Not hooks, and in every build: DUPEREMOVE_SCAN_STATS (a diagnostic) and
 * DUPEREMOVE_NO_LAYOUT_COPY (the layout copy's kill switch).
 */
#ifndef OANS_TEST_HOOKS
#define OANS_TEST_HOOKS 1
#endif

static inline const char *test_hook_env(const char *name)
{
#if OANS_TEST_HOOKS
	return getenv(name);
#else
	(void)name;
	return NULL;
#endif
}

#endif	/* __TEST_HOOKS_H__ */
