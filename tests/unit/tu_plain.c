/*
 * The subjects that need no static: pure functions reached through their
 * own headers, so this TU links against the sources like any consumer.
 */
#include "suite.h"

/* filerec.c is linked, so these reach only its public API. */
#include "test_filerec.c"
#include "test_util.c"
#include "test_longpath.c"
#include "test_storage.c"
#include "test_dedupe.c"
