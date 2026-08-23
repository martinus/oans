/*
 * The io-threads heuristic and the line describing it.
 *
 * Its own translation unit since the sysfs test reaches read_rotational_at(),
 * which is static: the seam is a parameter on an internal function, not a knob
 * on the product.
 */
#include "suite.h"

#include "storage.c"

#include "test_storage.c"
