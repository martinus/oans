#ifndef	__RUN_DEDUPE_H__
#define	__RUN_DEDUPE_H__

#include <stdbool.h>
#include "opt.h"

void print_dupes_table(struct results_tree *res, bool whole_file);
void dedupe_results(struct results_tree *res, bool whole_file);

/* Close the dedupe phase (which runs dedupe_results() many times): stops the
 * live status (see pdedupe_begin()) and prints one aggregated summary. */
void dedupe_end(void);

int fdupes_dedupe(void);

#endif	/* __RUN_DEDUPE_H__ */
