---
name: issues
description: Works every open GitHub issue to done, grouped into PRs by subject, each merged when CI is green, re-reading the issue list after every merge. Use when the reader says "do the issues", "work the issues", or asks to keep going until nothing is left.
argument-hint: "[optional: an issue number or subject to start with]"
---

# Do the issues

- Read every open issue, finish them all, stop when none is left.
- **One subject is one PR.** Do not move on until that PR is merged and the
  issue list is read again.
- "Do the issues" permits merging: **merge your own PR once CI is green**, and
  only then. This is the one exception to `CLAUDE.md`'s "merge it" rule; it
  ends with the loop and relaxes nothing else.
- **This file is the order of work. The rules are in `CLAUDE.md` and
  `.claude/rules/`**, and are not restated here: one rule in two files drifts.

## One pass

### 1. Read them all, then group

- `list_issues` (owner `martinus`, repo `oans`): every open issue, before
  touching anything.
- Group by **what they touch**, not by number: three issues in `src/glob.c`
  are one PR, two issues in one function are one commit. One PR closing four
  related issues beats four PRs conflicting in `src/file_scan.c`.
- Order:
  - Fixes that put wrong data into the hashfile first: a wrong digest stays
    until the file changes.
  - A change others build on before them.
  - A change that moves much code before the small ones it would rewrite.
- State the grouping and order before starting, one line each.

### 2. Reproduce before you fix

The report was often not the cause:

| Reported | Actually |
| --- | --- |
| dedupe reclaims the same bytes every run (#186) | three causes: `clean_deduped()` culled per physical offset, `fiemap_maps_share()` compared records not coverage, a trailing partial block never matched |
| WAL grows huge (#261) | a read transaction stayed open through hashing, so no checkpoint restarted the WAL |
| slow SQLite temp table (#260) | an `IN (...)` set filled in random key order rewrites ~1 temp page per row |
| flaky under valgrind (#197) | two dedupe windows elected different targets; a later one deduped onto storage an earlier one was moving (fixed in #199) |
| fails on a loaded CI runner (seen on #266) | the signal-raising thread paused between its count and `raise()` (fixed in #267) |

- Measure, print, run: a scan question by a scan, SQL by `EXPLAIN QUERY PLAN`
  and `scripts/bench-queries.py`, a layout by `filefrag -v`. **A fix for a
  guessed cause is a second bug.**
- No btrfs/XFS on the box: a scratch copy where `dedupe_probe_fd()` answers
  `DEDUPE_SUPPORT_YES` runs the whole scan and dedupe pipeline on ext4
  (`.claude/rules/mutation-testing.md`). Never scan `/tmp` (tmpfs).

### 3. Ask before you build, never after

- Offer two or three options with their real cost, one marked recommended.
- **Ask one level up when an issue asks for something the rules rule out**
  (an item in `.claude/rules/dead-ends.md`, any "don't" rule). If a rule must
  change, that is the question, and the rule is rewritten in the same PR.
- Stop and ask about:
  - a hashfile schema change, above all a `DB_FILE_MINOR` bump, which makes
    every user rescan (`.claude/rules/hashfile-schema.md`);
  - a new dependency;
  - a new command-line option, a changed default, or a changed exit status;
  - a change to what `--json`, `--progress=json` or `--history` print (other
    programs read them);
  - anything changing what `FIDEDUPERANGE` is asked to do, or which files a
    scan reaches;
  - an edit to the vendored `scripts/mutate/mutate_core.py` (a three-repo
    errand, `.claude/rules/mutation-testing.md`);
  - any choice where two readings lead to different work.
- Decide everything else yourself and say what you decided.

### 4. Build it

- A rule file loads by itself only when you open a path it lists. **Grep
  `.claude/rules/` for the file or symbol you will touch and read the hits
  before editing.**
- Prove every new test by breaking the code it guards: add a block to
  `scripts/mutate/bugs/<source>.txt`, run
  `scripts/mutate/mutate.py --bugs scripts/mutate/bugs/<source>.txt`. A block
  no test catches means the test cannot fail (`mutation-testing.md`, **What
  eight iterations of this actually taught**).
- Read the count, not the colour:
  - `./test` prints `N tests, M assertions, F failures`. A test missing from
    the `MU_RUN` list never runs; `make lint` checks the list.
  - The integration suite prints `skipped=K`; a dedupe test skips without
    reflink. On such a box compare the failure list with an `origin/master`
    build and let CI's btrfs and XFS jobs be the real check.

### 5. Write the scar down in the same commit

| Where | What |
| --- | --- |
| `.claude/rules/<subject>.md` | a rule the next agent needs |
| `scripts/mutate/bugs/*.txt` | the bug put back, so a test must catch it |
| `docs/man/oans.md`, then `make doc` | user-visible: option, exit status, output |
| `README.md` | anything a new reader needs before the man page |

The rule and the code that keeps it land together. `CLAUDE.md` (**Subsystem
rules**) says which file and bullet.

### 6. Review when it is worth it

Judgement, not ceremony; a two-line fix needs neither.

- **`/code-review`** before a PR touching the dedupe phase and its lifetimes,
  the hashfile schema, a `GET_DUPLICATE_*` / `COUNT_*` statement, signal
  handling, or how file names are printed; also any diff over a few hundred
  lines.
- **`/simplify`** after a pass that added much code, or a third spelling of
  something the codebase already names.

Fix findings before the PR goes up. Say which you ran.

### 7. Checks, push, watch CI

Before the push, run in the background what the rules ask for the code you
touched; write the scar and PR text meanwhile:

- `WERROR=1` builds with **both** gcc and clang, `./test` after each.
- `make lint`.
- `make mutation-replay`, or `mutate.py --bugs` for the changed sources' bug
  files.
- The integration suite; on the dev box `scripts/verify.sh` is the gate.
- Dedupe phase or lifetime change: `make integration-valgrind`.
- SQL: `scripts/bench-queries.py` against `origin/master` must say `same`.

**CI is the gate, not local green**: it alone caught #267's thread race,
#199's target race and a clang + `-O2` build. A red CI on your PR is work now:
find the cause, push until green. While CI runs, read and reproduce the next
issue, read-only: the branch stays as CI saw it.

### 8. Merge, reset, read the list again

Green means merge. Reset the working branch onto `master` and **read every
open issue again**; new ones arrive while you work. Stop watching the merged
PR and cancel any check-in armed for it.

## The pull request loop

Same calls every time. Owner `martinus` and repo `oans` are separate fields.

1. **Push**: `git push -u origin <branch>`.
2. **Create**: `create_pull_request`, head `<branch>`, base `master`, the body.
   The server adds a footer and a session link.
3. **Remove the footer**: `update_pull_request` with the same body, then
   `pull_request_read` method `get` and read the body back (`CLAUDE.md`,
   **Workflow**, "No Claude attribution anywhere").
4. **Watch**: `subscribe_pr_activity`, plus a `send_later` ~6 minutes out; a
   green run may send no event.
5. **Read CI**: `pull_request_read` method `get_check_runs`. Done = all 12
   runs completed, none failed: `build-and-test` on btrfs and XFS with gcc and
   clang, `valgrind` 1-4, `sanitizers` (address, undefined, thread),
   `mutation`. For a red one: `actions_list` method `list_workflow_jobs` with
   the run id as `resource_id`, then `get_job_logs` with `job_id`,
   `return_content: true`, `tail_lines`.
6. **Merge**: `merge_pull_request`, `merge_method: "squash"`,
   `commit_title: "<PR title> (#<n>)"`, `expectedHeadSha` from
   `git rev-parse HEAD` (all 40 characters). A 409 "Head branch was modified"
   means the head is not the one CI checked: re-read the checks on the new
   head, never force.
7. **Let go**: `delete_trigger` the check-in, `unsubscribe_pr_activity`.
8. **Back onto master**:

   ```sh
   old=$(git rev-parse HEAD)
   git fetch origin master
   git diff origin/master "$old" --stat   # must be empty: all of it merged
   git checkout -B <branch> origin/master
   git push --force-with-lease=<branch>:"$old" -u origin <branch>
   ```

   A squash merge is a new commit, so the push needs force; the lease replaces
   only the merged head. A non-empty diff means something did not land: stop.

## While looping

- **A bug reported in prose outranks the issue list** ("the progress bar is
  stuck"): someone is looking at it now. Reproduce, fix, fold into this pass.
- **Something found on the way**: do it now if small or in code you are
  already changing (one line from your fix does not widen the PR). Otherwise
  file an issue with the repo's labels (`bug`, `priority: high|medium|low`,
  and `robustness`, `security`, `performance`, `documentation` or
  `investigation` where they fit) and leave it. Trivia is not an issue.
- **Never guess a layout, a plan or a kernel answer.** Run `filefrag -v`,
  `EXPLAIN QUERY PLAN`, or the ioctl.
- **Say what you left out, and why.**

## When it is over

Nothing open, `master` green. Report which issues closed in which PR, what you
filed, what you chose not to do, and what still waits on the reader. A release
is not part of the loop: `scripts/release.sh` runs only when the reader asks.
