#ifndef	__RUN_DEDUPE_H__
#define	__RUN_DEDUPE_H__

#include <stdbool.h>
#include "opt.h"

void print_dupes_table(struct results_tree *res, bool whole_file);
void dedupe_results(struct results_tree *res, bool whole_file);

/* Bracket the whole dedupe phase (which runs dedupe_results() many times):
 * dedupe_begin() starts the single in-place status line and resets totals;
 * dedupe_end() stops it and prints one aggregated summary. */
void dedupe_begin(void);
void dedupe_end(void);

int fdupes_dedupe(void);

#endif	/* __RUN_DEDUPE_H__ */
