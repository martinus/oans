/*
 * The scan pipeline: the walk, the live progress block, and the extent search.
 *
 * These three share a translation unit because their tests reach into each
 * other's statics - a progress test drives file_scan's work queue, and the
 * search test asserts on progress.c's counters - and a source can be
 * #included into exactly one TU.
 */
#include "suite.h"

#include "file_scan.c"
#include "progress.c"
#include "find_dupes.c"

/*
 * find_dupes before progress: a progress test drives the extent search
 * through that file's fixtures, and C wants them declared first.
 */
#include "test_file_scan.c"
#include "test_find_dupes.c"
#include "test_progress.c"
