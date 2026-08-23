/*
 * The hashfile and the in-memory model it loads into.
 *
 * The filerec tests are deliberately *not* here: filerec.c is linked rather
 * than #included, so they reach nothing static and only made the largest TU
 * larger - measured, 130 ms of the critical path at -O2.
 *
 * One translation unit because the dedupe-phase loaders are where the two
 * meet: a loader test asserts on the results tree through find_dupe_extents(),
 * which is static in results-tree.c, and a source can be #included into
 * exactly one TU. Splitting them would mean exiling that test to a file it
 * does not belong in.
 */
#include "suite.h"

#include "dbfile.c"
#include "hash-tree.c"
#include "results-tree.c"

#include "test_hash_tree.c"
#include "test_dbfile.c"
