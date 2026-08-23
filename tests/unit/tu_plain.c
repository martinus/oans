/*
 * The subjects that need no static: pure functions reached through their
 * own headers, so this TU links against the sources like any consumer.
 *
 * A translation unit of the oans unit suite. Rebuilding this rebuilds only
 * these subjects, which is what makes a mutation sweep affordable.
 */
#include "suite.h"


#include "test_util.c"
#include "test_longpath.c"
#include "test_storage.c"
#include "test_dedupe.c"
