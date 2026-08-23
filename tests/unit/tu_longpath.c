/*
 * Paths past PATH_MAX (#117).
 *
 * Its own translation unit since the chunk-boundary property reaches
 * chunk_end(), which is static: the arithmetic is the part worth asking about
 * over every path shape, and it is only observable from inside longpath.c.
 */
#include "suite.h"

#include "longpath.c"

#include "test_longpath.c"
