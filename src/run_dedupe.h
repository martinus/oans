#ifndef	__RUN_DEDUPE_H__
#define	__RUN_DEDUPE_H__

#include <stdbool.h>
#include "opt.h"

void print_dupes_table(struct results_tree *res, bool whole_file);

/*
 * Streaming dedupe phase (Stage 2). One thread pool serves the whole phase; the
 * caller is a bounded producer that loads batch i+1 while batch i dedupes.
 *
 * Lifecycle:
 *   dedupe_phase_begin(on_complete);
 *   for each generation window (lo, hi]:
 *       dedupe_await_slot();                 // block until an in-flight slot frees
 *       batch = dedupe_begin_batch(hi);
 *       // load whole-file groups into dedupe_batch_files(batch), then:
 *       dedupe_push(batch, true);
 *       // load extent groups into dedupe_batch_extents(batch), then:
 *       dedupe_push(batch, false);
 *       dedupe_seal_batch(batch);            // enqueue + reap completed batches
 *   dedupe_phase_end();                      // drain, tear down, print summary
 *
 * on_complete(seq_hi) is called on the producer thread as each batch is reaped,
 * in generation order, to advance the durable dedupe_seq. All the batch calls
 * must happen on the single producer thread.
 */
struct dedupe_batch;

void dedupe_phase_begin(void (*on_complete)(unsigned int seq_hi));
void dedupe_phase_end(void);

void dedupe_await_slot(void);
struct dedupe_batch *dedupe_begin_batch(unsigned int seq_hi);
struct results_tree *dedupe_batch_files(struct dedupe_batch *b);
struct results_tree *dedupe_batch_extents(struct dedupe_batch *b);
void dedupe_push(struct dedupe_batch *b, bool whole_file);
void dedupe_seal_batch(struct dedupe_batch *b);

#endif	/* __RUN_DEDUPE_H__ */
