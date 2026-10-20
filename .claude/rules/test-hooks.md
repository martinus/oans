---
paths:
  - "src/**/*.{c,h}"
  - "tests/integration/harness.py"
  - "scripts/lint-test-hooks.py"
---

# Test hooks

- Read every new `DUPEREMOVE_*` hook with `test_hook_env()`
  (`src/test_hooks.h`, #295), **never `getenv()`**: only those drop out of
  `make TEST_HOOKS=0`.
  - `scripts/lint-test-hooks.py` (in `make lint`) flags a bare `getenv()`; CI
    builds the hook-less binary and fails on any hook name left in it.
  - List the name in the harness's `TEST_HOOK_VARS`, so the suite skips the
    test on a hook-less binary instead of running it with the hook ignored.
  - A hook written through a pointer (`env_uint()`) also needs an
    `#if OANS_TEST_HOOKS` constant, or the code behind it is not provably dead.
- Only `DUPEREMOVE_SCAN_STATS` and `DUPEREMOVE_NO_LAYOUT_COPY` are read in
  every build.
