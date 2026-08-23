/*
 * The hashfile and the in-memory model it loads into.
 *
 * One translation unit because the dedupe-phase loaders are where the two
 * meet: a loader test asserts on the results tree through find_dupe_extents(),
 * which is static in results-tree.c, and a source can be #included into
 * exactly one TU. Splitting them would mean exiling that test to a file it
 * does not belong in.
 *
 * A translation unit of the oans unit suite. Rebuilding this rebuilds only
 * these subjects, which is what makes a mutation sweep affordable.
 */
#include "suite.h"

#include "dbfile.c"
#include "hash-tree.c"
#include "results-tree.c"

#include "test_hash_tree.c"
#include "test_filerec.c"
#include "test_dbfile.c"
