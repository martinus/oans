---
paths:
  - "src/**/*.{c,h}"
  - "tests/integration/harness.py"
  - "scripts/lint-test-hooks.py"
---

# Test hooks

- **A new `DUPEREMOVE_*` test hook is read with `test_hook_env()`**
  (`src/test_hooks.h`, #295), never `getenv()`. `make TEST_HOOKS=0` builds
  without the hooks, and that is only true of the ones read that way:
  `scripts/lint-test-hooks.py` (in `make lint`) flags a bare `getenv()`, CI
  builds the hook-less binary and fails on any hook name left in it, and the
  harness's `TEST_HOOK_VARS` must list the new name so the suite skips the
  test on such a binary instead of running it with the hook ignored. A hook
  whose variable is written through a pointer (`env_uint()`) also needs an
  `#if OANS_TEST_HOOKS` constant, or the code behind it is not provably dead.
  Only `DUPEREMOVE_SCAN_STATS` and `DUPEREMOVE_NO_LAYOUT_COPY` are read in
  every build.
