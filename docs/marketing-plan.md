# oans launch plan

_Not for committing to the public repo — this is a working doc._
Target launch week: **Mon 2026-07-27 → Fri 2026-07-31.** Status verified against
the live repo on **2026-07-26**.

_(Already slipped once, 07-20→07-26, because the code kept moving. It is still
moving: **v1.5.0, v1.5.1 and v1.6.0 all shipped in the 30 hours after this doc
was last updated.** That is the failure mode — see [Code freeze](#code-freeze)
below, which is now the most important section here.)_

---

## TL;DR

1. **Zero code is blocking. Three chores are, and all three need you.** Enable
   GitHub Discussions, publish to the AUR, set the social preview image. Nothing
   else on the P0/P1 list is open; there are **no open PRs** and master is
   released as **v1.6.0**.
2. **Freeze the code today.** Every improvement you find between now and Friday
   goes in an issue, not in master. You are past the point where more polish buys
   reach — you have 2 stars, which means the bottleneck is that nobody knows the
   tool exists, not that it isn't good enough.
3. **Stagger, don't blast.** One channel per day, starting with the friendliest
   (r/btrfs), so you can absorb feedback and fix quick issues before the big-reach
   posts. Being present in the comments is what actually drives ranking.
4. **Lead with two numbers and the safety guarantee, be upfront about the
   AI-assisted development.** "7× faster warm re-runs" answers *why re-run it*;
   "~13× faster on a larger-than-RAM first run, byte-for-byte identical output"
   answers *why the first run isn't the whole story* — that second number is
   newer, stronger and better-documented than this doc used to give it credit
   for. Then "the kernel byte-compares every range, so a bug can waste work but
   can't corrupt data" defuses the objection you're guaranteed to get.

---

## Positioning

**One-liner:** _oans is a duperemove fork built for dedup on trees too big to fit
in RAM, re-run on a schedule (NAS, backup target, build server) — fast, hands-off,
and honest about what it freed._

**Audience, in priority order:**
1. btrfs users with a lot of data (r/btrfs, r/DataHoarder)
2. Self-hosters / homelab / NAS owners (r/selfhosted, r/homelab)
3. Linux storage nerds & the "why not ZFS" crowd (HN, Phoronix, openSUSE/Fedora)

**The four things that make it different (use these everywhere):**
- **Fast where it counts:** warm re-runs skip everything already hashed _and_
  already shared — the deduped-2M-file benchmark is ~92 s vs ~11 min upstream
  (~7×).
- **Fast on the *first* run too, when the tree is bigger than RAM** — i.e. every
  real NAS. Streaming dedupe + page-cache prefetch: **13.8 s vs 179.7 s (~13×)**
  at ~2× lower peak RSS, and `btrfs filesystem du -s` proves both tools leave the
  **byte-for-byte identical** on-disk layout. Use this whenever someone says "your
  benchmark is just a warm cache" — it's the honest answer and it's *stronger*
  than the 7×.
- **Set-and-forget:** the hashfile remembers your paths/options/excludes; bare
  `oans --hashfile=FILE` replays incrementally, and there's a systemd timer.
- **Honest & safe:** live progress with rate+ETA, a summary that reports disk
  actually freed (not inflated shared-extents), and dedup goes through the
  kernel's byte-verifying `FIDEDUPERANGE` — it can't corrupt data.

**One extra hook, fresh in v1.6.0 — use it with the NAS/homelab crowd.** Sparse
files whose size wasn't a block multiple were **silently skipped on every run**
(#152) — that's VM images, database files and preallocated media, i.e. exactly
what that audience stores. If they have those files, upgrading dedups them for
the first time. It's a concrete "here's what you get today", and it doubles as
evidence the project finds and fixes its own bugs.

---

## Code freeze

**This is the section that decides whether the launch happens.** Everything else
in this doc has been true and actionable for two weeks; what keeps changing is
the code, and each change resets the "let me just finish this one thing" clock.

The evidence, from this repo's own log: this doc slipped 07-20→07-26 because
"the code kept moving", and in the 30 hours *after* that note was written,
**#137, #138, #139, #140, #141, #142, #143, #144, #151, #152, #153, #154 and
#155 merged and three releases shipped.** Every one of them was a genuine
improvement. That is precisely why the freeze has to be a rule and not a
judgement call — you will never fail to find another real improvement, so
"is this worth delaying for?" is a question that always answers yes.

**The rule, from now until Fri 2026-07-31:**

- **master is frozen at v1.6.0.** No merges, no releases.
- **Anything you find goes in a GitHub issue**, immediately, with the
  reproduction. That is not losing the work — it's a public backlog, which on
  launch week is itself a signal the project is alive.
- **Two exceptions only:** data loss, or the tool fails to build/run for a new
  user on a normal machine. Nothing else qualifies. A perf win doesn't. A
  cosmetic progress-bar glitch doesn't. A missing FAQ entry is a README edit, not
  a release.
- **"I'll post right after this one PR" is the trap.** There is no such PR.
- **A bug found *during* launch week is an asset, not an embarrassment** —
  "good catch, fixed in <commit>" in a live thread is the best advertising you
  will get all week. That's an argument for posting sooner, not for polishing
  longer. See [After you post](#after-you-post).

The honest cost-benefit: another week of improvements moves the tool maybe 2%.
The first r/btrfs post moves it from 2 stars to a real user count. **The tool has
been launch-ready since v1.2.0; it is now three minor releases past that.**

---

## Before you post — pre-launch checklist

Prioritised. **[done]** items are merged to master; **[you]** still needs a human
(an asset, an account, or a setting I can't change from here). Status verified
against the live repo on **2026-07-26**.

### P0 — do these or the launch underperforms

- **[done] Ship a release.** ✅ **v1.6.0 is out** (2026-07-26 13:47), with the
  prebuilt `oans-1.6.0-linux-x86_64.tar.gz` + `.sha256` attached, and master is
  **clean — zero open PRs**. This item asked for v1.5.0; you shipped 1.5.0, 1.5.1
  *and* 1.6.0 in a day. **Launch against v1.6.0 and stop here** — "out this week"
  is as good a hook as "out today", and another release is another week.
- **[you] Enable GitHub Discussions.** Confirmed still **off** today, but
  `.github/ISSUE_TEMPLATE/config.yml` sends "Question or usage help" to
  `/discussions` — a dead link on exactly the surface a launch drives traffic to,
  and usage questions are the single most common thing a launch generates. One
  toggle in Settings → Features.
- **Make install one command.** From-source `make && sudo make install` alone
  loses a chunk of r/btrfs and r/DataHoarder.
  - **[done]** In-tree AUR `PKGBUILD`s: `packaging/aur/oans` (release) and
    `oans-git` (master), each with a `duperemove` provides/conflicts. Groundwork
    is committed and the README points at it.
  - **[you] Still not published — re-checked today, `aur.archlinux.org/packages/oans`
    and `oans-git` both still 404.** This is the last real gap in the install
    story; until it's done, "one-command install" is really "clone and makepkg".
    Needs your AUR account + SSH key: `cd packaging/aur/oans && makepkg
    --printsrcinfo > .SRCINFO` then push to `ssh://aur@aur.archlinux.org/oans.git`.
    (See `packaging/aur/README.md`.) The blocker this item used to have is gone —
    v1.6.0 is tagged, so **point the PKGBUILD at `v1.6.0` and publish now.**
  - **[done]** Prebuilt x86_64 binary attached to every release automatically
    (`.github/workflows/release.yml`, PR #75) — **verified working**: v1.2.0,
    v1.3.0 and v1.4.0 all carry `oans-X.Y.Z-linux-x86_64.tar.gz` + `.sha256`, so
    the backfill note here is obsolete. (A Copr/OBS build for Fedora/openSUSE
    stays optional.)
- **[done] Demo GIF in the README.** A scripted, reproducible screencast
  (`scripts/demo/`, PR #76) records scan → dedupe → `--stats`/`--history` and is
  embedded as the hero image at the top of the README. Your single best piece of
  post collateral — done.
- **[done] "How it compares" FAQ (bees / ZFS / duperemove)** added to the README
  and the man page NOTES, so you can link instead of retyping.

### P1 — strongly recommended

- **[you] Custom social preview image** (Settings → Social preview, 1280×640).
  **Re-checked today: still not set** — `usesCustomOpenGraphImage` is `false`, so every
  reddit / HN / Mastodon / Lemmy link renders GitHub's generic auto-card. This is
  the single most-viewed asset of the whole push and it's ~15 minutes: a
  *good-enough* static export of the mountain wordmark plus the one-liner beats
  the auto-card — it doesn't need to be a perfect vector. You already have
  `assets/logo.png` and the demo GIF to cut from.
- **[done] Preempt the AI-assisted skepticism.** The README's attribution
  paragraph now sits the byte-verified-kernel safety guarantee right next to the
  "developed with AI tooling" line.
- **[done] Discovery topics** added: `nas`, `homelab`, `selfhosted`, `storage`,
  `linux` (on top of btrfs/xfs/dedupe/duperemove/reflink/filesystem).
- **[done] Confirm the XFS claim.** CI now runs a `scratch_fs: [btrfs, xfs]`
  matrix (`.github/workflows/ci.yml`): the full integration suite runs against an
  `mkfs.xfs -m reflink=1` loopback image as well as btrfs, so the XFS support
  claim is CI-proven, not just manually smoked. The XFS leg runs unprivileged, so
  it also exercises the unprivileged `FS_IOC_GETFSUUID` path (Linux 6.4+); the
  harness auto-skips the few btrfs-only (fragmentation) tests on XFS.
- **[done] Sanitizers in CI — new since this plan was written, and it upgrades
  your best answer to the AI-skepticism question.** CI now runs clang **ASAN**,
  **UBSAN** and **ThreadSanitizer** legs over both test suites (#125, #127, #129,
  #131), builds with warnings-as-errors, and still runs the valgrind leg — all on
  a real btrfs *and* XFS. TSAN found a genuine use-after-free, which was fixed
  (#129). Say "ASAN/UBSAN/TSAN and valgrind clean in CI on real btrfs and XFS"
  wherever the drafts below say "valgrind-clean".

### P2 — nice to have, not blocking

- **[done] CONTRIBUTING.md + issue templates** (bug/feature YAML forms +
  Discussions/NAS-guide contact links) landed. ⚠️ The Discussions contact link
  is dead until you flip the toggle — see P0.
- **[skip] CHANGELOG** — leaning on the GitHub release notes (v1.0.0→v1.5.0 are
  already there, each with grouped notes); a separate file is maintenance for
  little gain.
- **[done] Benchmark methodology note** — `docs/benchmarks.md` documents the
  exact tree, cold-vs-warm commands and the duperemove 0.15.2 comparison, linked
  from the README so the 7× number is defensible.

---

## Where to post

| Channel | Fit | Reach | Rules / notes |
|---|---|---|---|
| **r/btrfs** | ★★★ | small, dense | Your home crowd. Technical, receptive, forgiving of a "I made a tool" post. **Launch here first** to shake out feedback. |
| **r/DataHoarder** | ★★★ | large | Dedup = free space; extremely on-topic. Read their self-promo rules; frame as show-and-tell + engage, don't drive-by. |
| **r/selfhosted** | ★★★ | large | NAS/homelab dedup is squarely on-topic. Often has a specific self-promo day/flair — check the sidebar. |
| **r/homelab** | ★★ | large | Good follow-up a couple days later; more hardware-flavored, but the NAS angle lands. |
| **Hacker News (Show HN)** | ★★ | large, spiky | "Show HN: oans – …". High reach, brutal comments (expect "why not ZFS/bees", and AI questions). Post a weekday US morning; be online to reply for the first 2–3 h. |
| **Phoronix** | ★★ | large | Michael Larabel covers btrfs/dedup tooling. Email a short tip / link the release; a Phoronix writeup outruns any single reddit post. |
| **openSUSE / Fedora forums + Mastodon (#btrfs #linux)** | ★★ | medium | openSUSE ships btrfs by default. Fediverse Linux crowd is real and friendly to honest FOSS. |
| **Lemmy** (`selfhosted`, `linux`) | ★ | growing | Cheap cross-post of the reddit content. |
| **btrfs mailing list** | ★ | niche | A brief, humble "fork announcement" is appropriate and earns upstream goodwill. |

Skip for now: r/linux (strict self-promo rules, big but noisy), r/truenas (ZFS-centric).

---

## When to post (week of 2026-07-27)

Stagger one primary channel per day so you're never answering two firehoses at
once. Post when the **US audience is waking up** — roughly **15:00–17:00 CET
(09:00–11:00 ET)**, **Tue–Thu** are best; avoid Fri afternoon → weekend.

| Day | Action |
|---|---|
| **Mon 07-27** | Prep only, no posting. **Three chores, ~1 h total:** enable Discussions (one toggle) → set the social preview (~15 min) → publish the AUR packages against `v1.6.0`. The release is already done. That's the whole remaining list. |
| **Tue 07-28** | **r/btrfs** ~15:00 CET. Watch comments all evening; fix quick issues, note FAQ gaps. |
| **Wed 07-29** | **r/DataHoarder** + **r/selfhosted** (morning apart, not simultaneously), using the r/btrfs feedback to sharpen the post. |
| **Thu 07-30** | **Show HN** ~15:00 CET + **Mastodon** + email the **Phoronix** tip. Clear your afternoon to reply on HN. |
| **Fri 07-31** | Light: **Lemmy** cross-post, **btrfs mailing list** note. Don't start a big thread going into the weekend. |
| **Following week** | **r/homelab**, openSUSE/Fedora forums as second-wave, once you've got stars/feedback to point at. |

**Don't let this slip again.** The Monday list is three human-only chores of
maybe an hour total, and not one of them is code. Baseline to measure against:
**2 stars, 1 fork, 1 watcher** on 2026-07-26. If this doc gets edited a third
time with a new target week, the problem is not the plan — see
[Code freeze](#code-freeze).

**Rule of thumb:** never post somewhere you can't babysit for the next 2–3 hours.
First-hour engagement is what decides whether a post ranks or dies.

---

## What to write (copy-paste drafts)

Keep it first-person, humble, technical. No marketing-speak — this audience
smells it instantly. Adjust to each sub's flair/rules.

### r/btrfs

> **Title:** oans — a duperemove fork focused on fast, repeatable dedup for big btrfs trees
>
> I run dedup on a large, mostly-stable btrfs tree on a schedule, and re-runs on
> duperemove got slow because it re-hashes and re-checks a lot it doesn't need to.
> So I forked it. **oans** skips everything already hashed _and_ everything already
> shared on a re-run — on a deduped ~2M-file / 230 GiB tree a warm re-run is ~92 s
> vs ~11 min with duperemove 0.15.2 here (~7×).
>
> That one's a warm cache, so the fairer number is the cold one: when the tree is
> **larger than RAM** — any real NAS — the kernel's dedup byte-compare re-reads
> from disk. oans streams the dedup phase and prefetches that read, so a *first*
> run under a 4 GiB cap is **13.8 s vs 179.7 s (~13×)** at ~2× lower peak RSS —
> and `btrfs filesystem du -s` says both tools leave byte-for-byte identical
> sharing. Method, raw rounds and repro script are in the repo.
>
> Other than speed it's built to be set-and-forget: the hashfile remembers your
> paths/options/excludes, so `oans --hashfile=FILE` with no other args replays
> incrementally, and there's a systemd timer for weekly idle-priority runs. It also
> has `--stats`/`--history`/`--json`, a live progress display with rate+ETA, and a
> summary that reports disk actually freed instead of an inflated shared-extents
> number.
>
> Dedup goes through the kernel's `FIDEDUPERANGE` ioctl, which byte-compares every
> range before sharing — so a bug can waste work or miss a dedup, but can't corrupt
> data. There's an integration suite CI runs against a real btrfs *and* XFS, under
> ASAN, UBSAN, ThreadSanitizer and valgrind.
>
> v1.6.0 (this week) also fixes a bug worth calling out: sparse files whose size
> isn't a multiple of the block size were silently skipped on *every* run — so VM
> images, DB files and preallocated media never got deduped at all. If you have
> those, this one finally hashes them. Heads-up for existing users: `--exclude`
> now uses `.gitignore` syntax instead of `fnmatch`, which is a breaking change,
> but the old behaviour matched at most one literal directory and usually nothing
> — silently. Check your patterns; there's a warning now when one matches nothing.
>
> Full credit to Mark Fasheh and the duperemove contributors — it's their engine.
> Fork improvements were developed with AI assistance and then reviewed, tested and
> benchmarked on real data before landing. Repo + benchmarks: <link>. Feedback very
> welcome — especially edge cases I haven't hit.

### r/DataHoarder / r/selfhosted

> **Title:** I forked duperemove so repeated dedup on a big NAS is actually fast (oans)
>
> _(same body, but open with the space-saving angle: "reclaimed 133 GiB across a
> media tree in 92 s on a re-run" — that's the real `Reclaimed 133.1 GiB across
> 164872 groups` line from the README's sample output, so it's quotable — then
> lean on the systemd-timer set-and-forget story, which is what this crowd wants.)_
>
> _Two things to promote for **this** audience specifically: (1) the sparse-file
> fix — VM images and preallocated media were being skipped entirely, and this
> crowd has exactly those; (2) the **larger-than-RAM** benchmark, because their
> trees are always larger than RAM and "13× on a first run, identical output" is
> the claim that survives contact with a 40 TB array. Say up front that
> `Reclaimed` is logical, not disk, on compressed btrfs — this sub will check
> with `compsize` and you want to have said it first._

### Show HN

> **Title:** Show HN: Oans – fast offline dedup for btrfs and XFS (duperemove fork)
>
> _(tighter, more technical. Lead with the mechanism — offline/batch extent dedup
> via `FIDEDUPERANGE`, incremental hashfile, skip-already-shared, streaming dedupe
> pipeline with prefetch — then the benchmark, then the safety guarantee. Have the
> bees/ZFS answers ready in the first comment; don't wait to be asked.)_
>
> _For HN specifically, **lead with the 13× larger-than-RAM number, not the 7×**.
> This crowd will immediately identify the warm re-run as a cache effect and say
> so; leading with the cold, memory-capped, byte-verified-identical benchmark
> pre-empts that and shows the methodology up front. "Both tools read the same
> 16.7 GiB and produce the same on-disk layout, verified with `btrfs filesystem
> du -s`; the difference is how the dedupe phase reads it back" is the sentence
> that earns the thread._

**Title tips:** put the concrete benefit and "duperemove fork" in the title (that's
the searchable hook), keep the fun name but don't rely on it to carry the click.

---

## Hard questions — have answers ready (and in the README FAQ)

These _will_ come up. Prewrite them; link instead of retyping.

- **"Why not bees?"** bees is a always-on daemon doing continuous block-level
  dedup across the whole filesystem; great for that. oans is offline/batch
  file+extent dedup you point at specific trees and run on a schedule — lower
  overhead when you don't want a resident daemon, and it gives you stats/history
  and honest per-run accounting. Different tool for a different workflow, not a
  competitor claim.
- **"Why not just ZFS dedup?"** ZFS dedup is inline and needs a big always-resident
  dedup table (RAM-hungry, hard to undo). oans is offline dedup for btrfs/XFS you
  already run — no dedup table, no permanent RAM cost, run it when you want.
- **"AI-assisted tool touching my data? no thanks."** Fair concern, so: dedup is
  performed by the kernel's `FIDEDUPERANGE`, which byte-compares every range before
  sharing — the tool literally cannot make the kernel share non-identical data.
  Worst case is wasted work or a missed dedup, both harmless. On top of that, CI
  runs the integration suite on real btrfs and XFS under ASAN, UBSAN,
  ThreadSanitizer and valgrind, with warnings as errors. That's not decoration:
  TSAN caught a real use-after-free (fixed in v1.5.0), valgrind-on-the-suite
  caught another, and v1.6.0 fixed a silent sparse-file skip and a miscounted
  dedupe summary — all found and fixed in the open, each with a regression test.
  Every change was reviewed, tested and benchmarked. **Don't be defensive here:**
  "here are the bugs we found and how" is a far better answer than "there are no
  bugs", and it's the one that's actually true of every codebase.
- **"Is the 7× real?"** Yes, with caveats, and volunteer them: it's a _warm
  re-run_ on an already-deduped 2M-file/230 GiB btrfs tree. **Then immediately
  give them the better number** — the larger-than-RAM benchmark is a *cold* first
  run under a hard 4 GiB cgroup cap, 10 interleaved rounds, median 13.8 s vs
  179.7 s, with both tools verified to produce byte-for-byte identical sharing.
  One machine, one NVMe, and on a slow HDD the gap would be *larger*, not smaller.
  Exact tree, commands and repro script: <link to methodology>.
- **"Why is the first-run gain so much bigger than the warm-run gain? That's
  backwards."** Because they're different bottlenecks. Warm re-runs win by
  *skipping* work already done (~7×). The larger-than-RAM win is the dedupe phase:
  `FIDEDUPERANGE` byte-compares every range, and when the tree doesn't fit in the
  page cache that compare re-reads from disk cold. oans streams the dedupe
  pipeline and prefetches, so the kernel compares from RAM (~13×). Same work,
  different read pattern.
- **"Should I upgrade from 1.5.x? Anything that'll bite me?"** Yes and yes, one
  thing: `--exclude` switched from `fnmatch`-on-the-full-path to `.gitignore`
  syntax in 1.6.0. That's deliberate and it's a fix — the old semantics matched at
  most one literal directory and usually nothing at all, *silently*, so
  `--exclude node_modules` did nothing. Patterns stored in an existing hashfile
  replay under the new meaning. Re-check your patterns; 1.6.0 warns when one
  matches nothing. Also in 1.6.0: sparse files with a non-block-multiple size were
  being skipped on every run and now aren't.
- **"Hashfile compatibility with duperemove?"** Intentionally not interchangeable
  (branded `application_id`); each rebuilds rather than misreads the other's. It's
  only a cache, so nothing is lost.
- **"On compressed btrfs the number looks too big."** `Reclaimed` is a _logical_
  figure; real disk freed is ~compression-ratio smaller because dedup shares
  logical extents. Verify with `compsize` Disk Usage before/after.

---

## After you post

- **Reply to everything in the first 2–3 hours.** Upvotes follow engagement.
- **Ship small fixes live.** "Good catch — fixed in <commit>" on launch day is the
  best possible advertisement.
- **Don't argue.** For "why not X", answer once, factually, and move on. The
  bees/ZFS debate is unwinnable and unnecessary — you're a different workflow.
- **Track:** stars/day, referral sources (GitHub Insights → Traffic), which
  channel converts, and recurring questions → fold them back into the README FAQ.
- **Roll feedback forward.** Whatever r/btrfs asks on Tue becomes a README/FAQ
  improvement before the r/DataHoarder and HN posts.
