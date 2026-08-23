/*
 * What every test file in this directory includes, and the one place the
 * suite's shape is decided.
 *
 * `MU_TEST` is redefined here, deliberately. Upstream minunit expands it to a
 * `static` function, which is right when the suite is one translation unit and
 * wrong now that it is one per subject: the runner in main.c has to name every
 * test, and a static one is invisible to it.
 *
 * Giving the tests external linkage rather than grouping them into per-subject
 * suite functions is not a style choice. Test order is load-bearing here -
 * every memdb() handle opens the *same* shared-cache in-memory database, so
 * "what does this answer against a fresh hashfile" has to be asked before
 * anything has stored a row. Regrouping the run order to match the file layout
 * would quietly reorder those. main.c keeps the original MU_RUN_TEST list, so
 * the suite still runs in exactly the order it always did.
 */
#ifndef OANS_TEST_SUITE_H
#define OANS_TEST_SUITE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/falloc.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sqlite3.h>
#include <glib.h>

/*
 * The oans headers the fixtures and the tests need. They used to arrive by
 * accident: with the whole suite in one translation unit, whatever the first
 * #included source pulled in was visible to everything after it. One TU per
 * subject ends that, so what is needed is named here once rather than
 * rediscovered by each test file.
 */
#include "csum.h"
#include "dbfile.h"
#include "filerec.h"
#include "hash-tree.h"
#include "results-tree.h"
#include "fiemap.h"
#include "file_scan.h"
#include "opt.h"
#include "util.h"
#include "debug.h"
#include "dedupe.h"
#include "glob.h"
#include "storage.h"
#include "longpath.h"
#include "interrupt.h"
#include "find_dupes.h"
#include "progress.h"
#include "threads.h"
#include "btrfs-util.h"
#include "list_sort.h"

#include "minunit.h"
#include "proptest.h"

#undef MU_TEST
#define MU_TEST(method_name) void method_name(void)

/* Set by main() from argv[0]; a few tests re-exec the binary. */
extern char *exec_path;

#include "fixtures.h"

#endif /* OANS_TEST_SUITE_H */
