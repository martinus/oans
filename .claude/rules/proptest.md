---
paths:
  - "src/proptest.h"
  - "tests/unit/**"
---

# `src/proptest.h` — properties, not tables

An ordinary test names an input and its answer; a property names a relationship
that must hold for *every* input and generates inputs looking for a
counterexample. No dependencies, one header, minunit-compatible.

- **The seed is fixed on purpose** (`make test` is reproducible, CI can trust
  it); `OANS_PROPTEST_SEED=random ./test` goes looking and prints what it chose,
  and every failure names the seed and case number to replay. Each property
  draws from its own stream, mixed from the seed and the test's name, so adding
  one does not renumber the cases in all the others.
- **There is no shrinking**, which is most of what a real property-testing
  library is. The substitute is generators that only produce small inputs, so a
  counterexample is already readable. A property needing a large input to mean
  anything should be an ordinary test with a fixture.
- **Three properties are skipped under valgrind** (`OANS_PROPTEST_NO_JIT=1`,
  which the CI leg sets and which prints what it skipped). They compile a
  `GRegex`, GLib compiles every pattern with `G_REGEX_OPTIMIZE`, and memcheck
  cannot see into the machine code PCRE2 then emits — hundreds of "conditional
  jump depends on uninitialised value" from frames with no symbol and a stack
  address where a return address belongs. **Verified to be the library and not
  oans**: a standalone program containing no oans code is clean under memcheck
  with plain `g_regex_new` and reproduces the identical signature the moment
  `G_REGEX_OPTIMIZE` is added. Don't reach for a suppression instead — the
  frames have no object name, so valgrind's own generated entry is
  `Memcheck:Cond` over `obj:*`, which would hide every uninitialised-value error
  in the process, and that check is what caught the missing memset in
  `start_running_checksum()`.
- **Keep the suite fast, for a reason beyond taste.** The mutation tool derives
  a mutant's hang timeout from how long a green run takes, so a slow suite
  reclassifies slow mutants as `hang` instead of letting a test catch them —
  measured: at 1.2 s two glob mutants moved from `caught`/`survived` to `hang`.
  The glob properties compile a `GRegex` per pattern, which is nearly all of
  what they cost, so they try `PROP_GLOB_PATHS` paths per compile. The whole
  suite is ~0.3 s for ~1M assertions; keep it there.
- **A property phrased in terms of the function under test cannot see that
  function being wrong**, only being inconsistent. The escaping property asserts
  `!has_ctrl(out)` — and `has_ctrl` *is* `ctrl_seq_len`, which is also what the
  escaper calls, so a mutation inside the classifier leaves both sides agreeing.
  Measured: narrowing the C1 test from `>= 0x80` to `> 0x80` was caught by
  nothing there, while U+0080 reached the terminal unescaped. The property now
  also checks the raw bytes, spelled out rather than delegated. Reach for a
  second, independent phrasing wherever the obvious property is a tautology
  waiting to happen.
- **They are not a replacement for the tables, and the lift is uneven.**
  Measured, same 413-mutant sweep of `src/util.c` with the properties removed
  and restored: **213 survivors → 203**, and every one of the ten is a
  `survived → caught` in `sanitize_ctrl`'s buffer arithmetic — the truncation
  boundary, which needs a buffer that runs out at each possible offset and so
  is exactly what a hand-written table does not cover. The same A/B over
  `src/glob.c` (305 mutants) gained **nothing**: two mutants moved, both from
  `caught`/`survived` to `hang`, purely because the suite had got slower. On
  the named-bug files they earn their place differently again — leaving the
  fiemap address set unsorted is caught by nothing else. Expect that
  unevenness rather than a uniform number.
  - **`longpath.c`'s chunk boundary is the second clear win, and it needed a
    seam first.** The split lived inside `open_ancestor()`, which opens each
    chunk as it goes, so asking what the split *is* meant building a real
    directory tree of the right shape - and the suite has to stay fast enough
    that the mutation tool's hang timeout still separates a slow mutant from a
    caught one. Pulled out as `chunk_end()`, a pure function, it is askable
    about every path shape for nothing: **65 survivors → 61, and `chunk_end`
    itself 4 → 0.** All four are the `LONGPATH_MAXLEN + 1` the comment there
    calls deliberate.
    - **The first draft left one of them alive, and the residue said which.**
      `<=` → `<` on the limit differs only at *exactly* `LONGPATH_MAXLEN`, and
      only matters for a path with **no separator at all** - there the mutant
      answers `ENAMETOOLONG` for a path that fits. The generator capped
      components at 300 characters, so it never made a slashless 4095-byte
      path. One `prop_chance` for that shape took it to zero. Read the residue
      by function and the generator's gap is what it names.
  - The three **hashfile** properties are the same story a third time:
    `src/dbfile.c` swept with and without them is **786 survivors → 785**,
    one mutant. That one is worth the three, though - `i > 0` → `i > 1` in
    `dbfile_layout_matches`, i.e. *a donor with exactly one stored extent
    stops matching*. Every fixture in the table has two records, so nothing
    there could see it, and a single-extent file is the common case: btrfs
    reports a contiguously allocated file as one fiemap record however large
    it is. A snapshot-aware scan would have silently stopped copying
    anything on the most ordinary layout there is.
  - **And the sweep cannot score one of them at all.** "Normalize adjacent
    records before comparing" - the #186 confusion, and what
    `test_prop_a_split_record_is_not_the_layout_it_covers` exists to refuse -
    is a design change, not a single-token edit or a statement deletion, so
    no operator generates it. Read a property's `caught` count as a lower
    bound on what it guards, never as the measure.

## Soaking the property tests

`OANS_PROPTEST_SCALE=100 ./test` multiplies every property's case count. A
multiplier rather than a fixed number, so the ordinary run keeps the count each
property chose - those are tuned, and the glob ones are shaped around what
compiling a `GRegex` costs.

- **A soak extends a run rather than replacing it.** Each property draws from
  its own stream, so the first N cases of a soak are exactly the cases an
  unscaled run would try. A failure at case 40,000 is something the default
  genuinely could not reach, and still names the seed and case to replay.
- **The wide pass is the half that teaches.** Six random seeds at scale 100 say
  the properties are not tuned to the default seed; the deep pass at scale 500
  mostly confirms, since more cases cannot find a bug in a property that is
  already a tautology - and this tree has had one of those.
- Measured clean at ~1.45 billion assertions across seven runs. Read that as
  "no rare input shape is missing from the generators", not as "the properties
  are good".
