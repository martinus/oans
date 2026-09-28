---
name: issues
description: Work every open GitHub issue to done — grouped into PRs by subject, each merged when CI is green, re-reading the issue list after every merge. Use when the reader says "do the issues", "work the issues", or asks to keep going until nothing is left.
argument-hint: "[optional: an issue number or subject to start with]"
---

# Do the issues

Read every open issue, finish them all, stop when nothing is left.

**One subject is one PR.** The loop does not move on until that PR is merged
and the issue list has been read again.

Saying "do the issues" is the permission to merge: **merge a PR of your own
the moment CI is green**, and only then. This is the one exception to
`CLAUDE.md`'s "merge it" rule, and it ends when the loop ends. Nothing else
is relaxed.

**This file is the order of the work. `CLAUDE.md` is the rules.** Where it
answers a question, it answers it here too, and it is not restated below. One
rule in two files is two rules that drift.

## One pass

### 1. Read them all, then group

`list_issues` (owner `martinus`, repo `oans`), every open one, before
touching anything. Group by **what they touch**, not by number: three issues
about `src/glob.c` are one PR, and two issues about one function are one
commit. A PR that closes four related issues beats four PRs that conflict in
the same 4,000-line `src/file_scan.c`.

Order the work:

- A fix that puts wrong data into the hashfile goes first. A wrong digest
  stays until the file changes, so every run before the fix adds more.
- A change others build on goes before them.
- A change that moves a lot of code goes before the small ones that it would
  rewrite.

Say the grouping and the order before you start. One line each.

### 2. Reproduce before you fix

In this repository the report was often not the cause:

| Reported | Actually |
| --- | --- |
| "dedupe does not converge: same bytes reclaimed on every run" (#186) | three separate causes: `clean_deduped()` culled per physical offset, `fiemap_maps_share()` compared records instead of coverage, and a trailing partial block could never match |
| "the WAL file can get insanely large" (#261) | a read transaction stayed open for the whole hashing phase, so no checkpoint could restart the WAL |
| "slow SQLite temp table" (#260) | an `IN (...)` set filled in random key order rewrites about one temp page per row |
| "a test is flaky under valgrind" (#197) | two dedupe windows elected different targets, and a later window deduped onto storage an earlier one was still moving (fixed in #199) |
| "a test fails on a loaded CI runner" (seen on #266) | the thread that raises the signal was paused between its count and `raise()` (fixed in #267) |

Measure it, print it, run it. A scan question is answered by a scan, a SQL
question by `EXPLAIN QUERY PLAN` and `scripts/bench-queries.py`, a layout
question by `filefrag -v`. **A fix for a cause you guessed is a second bug
on top of the first.**

Where the box has no btrfs or XFS: a scratch copy of the tree where
`dedupe_probe_fd()` answers `DEDUPE_SUPPORT_YES` runs the whole scan and
dedupe pipeline on ext4 (`CLAUDE.md`, **Mutation & property testing**).
Never scan `/tmp`: it is tmpfs.

### 3. Ask before you build, never after

Two or three options, each with its real cost, one marked recommended. A
design question answered after the code is written is a question you
answered yourself.

**Ask one level up when an issue asks for something `CLAUDE.md` rules out**:
an item under **Measured dead-ends**, or a "don't" rule anywhere. If a rule
has to change, that is the question, and the rule is rewritten in the same
PR.

Stop and ask about:

- a change to the hashfile schema, above all a `DB_FILE_MINOR` bump, which
  makes every user rescan (**Hashfile identity & schema version**);
- a new dependency;
- a new command-line option, a changed default, or a changed exit status;
- a change to what `--json`, `--progress=json` or `--history` print, since
  other programs read them;
- anything that changes what `FIDEDUPERANGE` is asked to do, or which files
  a scan reaches;
- an edit to the vendored `scripts/mutate/mutate_core.py` (a three-repo
  errand, per `CLAUDE.md`);
- any choice where two readings lead to different work.

Decide everything else yourself and say what you decided.

### 4. Build it

`CLAUDE.md` has one section per subject. **Grep it for the file or symbol
you are about to touch, and read that section before editing**, not after
the tests go red. The **Where to look** table at its top routes from a
source file to its sections.

Prove every new test by breaking the code it guards: add a block to
`scripts/mutate/bugs/<source>.txt` and run
`scripts/mutate/mutate.py --bugs scripts/mutate/bugs/<source>.txt`. A block
no test catches means the test cannot fail. `CLAUDE.md` (**What eight
iterations of this actually taught**) says why that happens more often than
it seems.

Read the count, not the colour:

- `./test` prints `N tests, M assertions, F failures`. A test missing from
  the `MU_RUN` list never runs; `make lint` checks the list.
- The integration suite prints `skipped=K`. A dedupe test skips on a
  filesystem without reflink, and a skipped test has not run. On such a box,
  compare the list of failures with a build of `origin/master`, and let CI's
  btrfs and XFS jobs be the real check.

### 5. Write the scar down in the same commit

| Where | What |
| --- | --- |
| `CLAUDE.md` | a rule the next agent needs, in the section for its subject |
| `scripts/mutate/bugs/*.txt` | the bug put back, so a test must catch it |
| `docs/man/oans.md` (then `make doc`) | anything a user of the program sees: an option, an exit status, an output |
| `README.md` | anything a new reader needs before the man page |

The rule and the code that keeps it land together, or the rule is not true
yet. **Grep `CLAUDE.md` for the symbol before you add a bullet**: the same
kind of bug again goes into the bullet that already holds it. A new source
file or subject gets a row in the **Where to look** table.

### 6. Review it when review is worth it

Judgement, not ceremony. A two-line fix needs neither.

- **`/code-review`** before you open a PR that touches the dedupe phase and
  its lifetimes, the hashfile schema or a `GET_DUPLICATE_*` / `COUNT_*`
  statement, signal handling, or how file names are printed. Also for any
  diff longer than a few hundred lines.
- **`/simplify`** after a pass that added a lot of code, or that wrote a
  third spelling of something the codebase already had a word for.

Fix what they find before the PR goes up. Say which you ran.

### 7. The checks, then push, then watch CI

Before the push, run what `CLAUDE.md` asks for the code you touched, in the
background, and write the scar and the PR text in the meantime:

- `WERROR=1` builds with **both** gcc and clang, and `./test` after each.
- `make lint`.
- `make mutation-replay`, or `mutate.py --bugs` for the bug files of the
  sources you changed.
- The integration suite. On the dev box, `scripts/verify.sh` is the gate.
- For the dedupe phase or any lifetime change: `make integration-valgrind`.
- For SQL: `scripts/bench-queries.py` against `origin/master`, and it must
  say `same`.

Green locally is **not** the gate; CI is. It caught a thread race that no
local run could (#267), a target race that only a loaded runner showed
(#199), and a clang + `-O2` build nobody had run locally.

A red CI on a PR you opened is work now, not news. Find the cause again and
push again until it is green. While CI runs, read the next issue and
reproduce it. Only read: the branch stays as CI saw it.

### 8. Merge, reset, and read the list again

Green means merge. Then reset the working branch onto `master`, and **read
every open issue again**: the reader files them while you work, and so do
you. The next pass starts from the list as it is now, not the list you
sorted an hour ago.

Stop watching the merged PR and cancel any check-in you armed for it.

## The pull request loop

The same calls every time. Owner `martinus` and repo `oans` go in separate
fields.

1. **Push**: `git push -u origin <branch>`.
2. **Create**: `create_pull_request` with head `<branch>`, base `master`, and
   the body. The server adds a footer and a session link to it.
3. **Take the footer off**: `update_pull_request` with the very same body,
   then `pull_request_read` method `get`, and read the body back. `CLAUDE.md`
   says why (**Repo layout & workflow**, "No Claude attribution anywhere").
4. **Watch**: `subscribe_pr_activity`, and a `send_later` about six minutes
   out, because a green run may send no event.
5. **Read CI**: `pull_request_read` method `get_check_runs`. Done means all
   12 runs completed and none failed: `build-and-test` on btrfs and XFS with
   gcc and clang, `valgrind` 1-4, `sanitizers` (address, undefined, thread)
   and `mutation`. For a red one: `actions_list` method `list_workflow_jobs`
   with the run id as `resource_id`, then `get_job_logs` with `job_id`,
   `return_content: true`, `tail_lines`.
6. **Merge**: `merge_pull_request` with `merge_method: "squash"` (this
   repository squashes), `commit_title: "<PR title> (#<n>)"`, and
   `expectedHeadSha` from `git rev-parse HEAD`, all 40 characters. A 409
   "Head branch was modified" means the head is not the one CI checked: read
   the checks again on the new head, never force.
7. **Let go**: `delete_trigger` the check-in, `unsubscribe_pr_activity`.
8. **Back onto master**:

   ```sh
   old=$(git rev-parse HEAD)
   git fetch origin master
   git diff origin/master "$old" --stat   # must be empty: all of it merged
   git checkout -B <branch> origin/master
   git push --force-with-lease=<branch>:"$old" -u origin <branch>
   ```

   A squash merge is a new commit, so the branch is not an ancestor of
   `master` and the push needs force. The lease makes sure that only the head
   that was merged is replaced. If the diff is not empty, stop: something on
   the branch did not land.

## While looping

- **A bug reported in prose outranks the issue list.** "The progress bar is
  stuck" goes to the top: somebody is looking at it right now. Reproduce it,
  fix it, and fold it into the pass you are on.
- **Something you find on the way**: if it is small, or it belongs to the
  code you are already changing, do it now. A fix one line from the one you
  are making does not widen the PR. Otherwise file an issue with the labels
  the repository uses (`bug`, `priority: high|medium|low`, and `robustness`,
  `security`, `performance`, `documentation` or `investigation` where they
  fit) and leave it. Trivia is not an issue.
- **Never guess a layout, a plan or a kernel answer.** Run `filefrag -v`,
  `EXPLAIN QUERY PLAN`, or the ioctl itself.
- **Say what you left out, and why.** A pass that skipped something without
  saying so reads as a pass that finished.

## When it is over

Nothing open, `master` green. Say which issues closed in which PR, what you
filed, what you decided not to do, and anything still waiting on the reader.
A release is not part of the loop: `scripts/release.sh` stays a step the
reader asks for.
