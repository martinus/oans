---
paths:
  - "src/filerec.{c,h}"
  - "src/hash-tree.{c,h}"
  - "src/results-tree.{c,h}"
  - "src/rbtree.c"
  - "tests/unit/test_filerec.c"
  - "tests/unit/test_hash_tree.c"
---

# Dedupe model unit tests

`filerec.c`, `hash-tree.c` and `results-tree.c` are pure memory and need no
fixture. 502 mutants: 0% → 73% killed (135 survivors) on nineteen tests;
`bugs/{filerec,hash-tree,results-tree}.txt` put nine invariants back.

- **`src/rbtree.c` gets no tests of its own**: it is verbatim `linux/lib`
  code, exercised through these modules. Generate insert *and erase* orders;
  an ascending sweep never rebalances.
- **`free_all_filerecs()` ignores `refs`.** "A filerec lives until its last
  reference" is `run_dedupe.c`'s batch-ref rule, not testable here. Test that
  `filerec_free()` erases the fileid node, so `filerec_find()` cannot return
  a dead pointer.
- **Contracts fixtures get wrong:**
  - `insert_result()` length is `endoff[0] - startoff[0] + 1`: inclusive,
    from the first pair only.
  - `remove_extent()` collapses a group that drops to one member: removing
    from a pair takes both extents and returns 0, not 1.
  - `remove_hashed_block()` returns 1 only when it freed the whole list
    (`find_dupes` then stops walking it).
- **Use several files.** A block list holds every file's blocks under one
  digest, so one file cannot tell "this file's share drained" from "the list
  was freed".
- **There is no per-(digest, file) index; don't add one.** `find_dupes` walks
  a file's blocks through `filerec->block_tree` (rbtree keyed on offset);
  other copies come from `dl_list`. The `file_hash_head` tree and its
  per-window `list_sort` were removed: nothing read them, and they cost a
  malloc and an rbtree insert per (digest, file) pair in partial mode.
- **Test the fd refcount with a read.** `filerec_open()` returns the
  already-open fd; the test reads a byte through it after an inner close.
- **Read the residue by function**: at 56% overall, three blocks had no kill
  at all, and two were code no test called (`insert_result`, the fd group).
