---
paths:
  - "src/filerec.{c,h}"
  - "src/hash-tree.{c,h}"
  - "src/results-tree.{c,h}"
  - "src/rbtree.c"
  - "tests/unit/test_filerec.c"
  - "tests/unit/test_hash_tree.c"
---

# The dedupe model is unit-testable too, and needs no fixture at all

`filerec.c`, `hash-tree.c` and `results-tree.c` are pure memory. They were at a
**0% kill rate** - 502 mutants, not one caught - and are now at **73%** (135
survivors) on nineteen tests, with `bugs/{filerec,hash-tree,results-tree}.txt`
putting nine of the invariants back.

- **They are the only callers of the vendored kernel rbtree**, which is why
  `src/rbtree.c` gets no tests of its own: it is
  verbatim `linux/lib` code with no oans-specific API, so sweeping it would
  measure upstream's design. Generated insert *and erase* orders here are what
  make that copy rebalance - an ascending sweep only ever unlinks a spine.
- **`free_all_filerecs()` ignores `refs`.** So "a filerec survives until its
  last reference goes" is *not* true of this module: that rule lives in
  `run_dedupe.c`'s batch refs and is not a unit-testable claim. What is
  testable is that `filerec_free()` erases the fileid node, so a later
  `filerec_find()` cannot hand back a dead pointer. A comment claiming the
  bigger thing shipped in the first draft and was caught by review.
- **Three contracts a fixture gets wrong first.** `insert_result()`'s length is
  `endoff[0] - startoff[0] + 1` - inclusive, and from the *first* pair alone.
  `remove_extent()` collapses a group that drops to one member, so removing
  from a *pair* takes both extents and answers 0 where a decrement would say 1.
  And `remove_hashed_block()` answers 1 only when it freed the whole list,
  which is how `find_dupes` knows not to keep walking it.
- **Several files, or the test sees nothing.** A block list holds every
  file's blocks under one digest, so a single-file fixture cannot tell "this
  file's share drained" from "the list was freed".
- **No per-(digest, file) index, and none is needed.** `find_dupes` walks a
  file's blocks through `filerec->block_tree`, an rbtree keyed on the offset,
  so offset order holds by construction; a digest's other copies come from
  `dl_list`. The `file_hash_head` tree and its per-window `list_sort` were
  upstream leftovers nothing read, a malloc and an rbtree insert per (digest,
  file) pair in partial mode; they were removed.
- **The fd refcount needs a read, not a counter.** `filerec_open()` hands back
  the descriptor already open; what separates that from close-every-time is
  that the fd stays *usable* across an inner close, so the test reads a byte
  through it.
- **Read the residue by function.** The first pass looked like an ordinary 56%,
  but three blocks had not moved by a single mutant - and two were code the
  tests never called (`insert_result`, the whole fd group). The total hid that
  completely; only the grouping showed it.
