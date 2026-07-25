#ifndef	__FIND_DUPES_H__
#define	__FIND_DUPES_H__

#include <stdbool.h>

#include "opt.h"
#include "results-tree.h"

/* from oans.c */
extern unsigned int blocksize;

int find_additional_dedupe(struct results_tree *extents);

/* True when no block-hash search worker is still running. find_additional_
 * dedupe() guarantees this on return; the dedupe producer asserts it before
 * freeing filerecs those workers would be reading (#123). */
bool extents_search_idle(void);

void extents_search_init(void);
void extents_search_free(void);
#endif	/* __FIND_DUPES_H__ */
