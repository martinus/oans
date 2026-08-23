/*
 * The oans C unit suite: one translation unit, on purpose.
 *
 * Every source below is #included rather than linked, because 25 of the
 * functions under test are `static` - among them compare_extents() and
 * record_match(), where writing their tests found a real bug in partial-mode
 * dedupe. A suite that could only reach what the headers declare would not
 * have had those tests at all.
 *
 * It also decides what `scripts/mutate/mutate.py` may sweep: a file this does
 * not include is not part of the binary the tests run, so every mutant in it
 * would come back `survived`. The tool refuses such a file rather than
 * reporting a 0% kill rate over code nothing was measuring.
 *
 * The test bodies live in tests/unit/test_*.c, one per subject, and are
 * #included here for the same reason - still one translation unit, still one
 * compile.
 */

#include <stdbool.h>
#include <sys/stat.h>

#include "minunit.h"
#include "proptest.h"
#include "rbtree.c"

#include "opt.c"
#include "util.c"
#include "debug.c"
#include "interrupt.c"
#include "csum.c"
#include "threads.c"
#include "btrfs-util.c"
#include "file_scan.c"
#include "filerec.c"
#include "dbfile.c"
#include "hash-tree.c"
#include "results-tree.c"
#include "list_sort.c"
#include "find_dupes.c"
#include "memstats.c"
#include "fiemap.c"
#include "progress.c"
#include "storage.c"
#include "longpath.c"
#include "glob.c"
#include "dedupe.c"


unsigned int blocksize = DEFAULT_BLOCKSIZE;
static char *exec_path;

/* Fixtures first: every test file below may use them. */
#include "fixtures.h"

/* The subjects, roughly in the order a run meets them. */
#include "test_file_scan.c"
#include "test_util.c"
#include "test_longpath.c"
#include "test_glob.c"
#include "test_csum.c"
#include "test_storage.c"
#include "test_progress.c"
#include "test_fiemap.c"
#include "test_filerec.c"
#include "test_hash_tree.c"
#include "test_dbfile.c"
#include "test_find_dupes.c"
#include "test_interrupt.c"
#include "test_dedupe.c"

MU_TEST_SUITE(test_suite) {
	MU_RUN_TEST(test_running_checksum_survives_save_restore);
	MU_RUN_TEST(test_running_checksum_repoints_the_secret_on_restore);
	MU_RUN_TEST(test_running_checksum_save_refuses_a_short_buffer);
	MU_RUN_TEST(test_running_checksum_rejects_foreign_state);
	MU_RUN_TEST(test_is_block_zeroed);
	MU_RUN_TEST(test_block_len);
	MU_RUN_TEST(test_is_file_renamed);
	MU_RUN_TEST(test_seen_inode);
	MU_RUN_TEST(test_get_extent);
	MU_RUN_TEST(test_fiemap_layout_key);
	MU_RUN_TEST(test_fiemap_maps_share);
	MU_RUN_TEST(test_fiemap_unshared_bytes);
	MU_RUN_TEST(test_fiemap_unshared_bytes_accumulates);
	MU_RUN_TEST(test_fiemap_phys_set_grows);
	MU_RUN_TEST(test_sanitize_ctrl);
	MU_RUN_TEST(test_progress_copy_path);
	MU_RUN_TEST(test_progress_path_two_stage_render);
	MU_RUN_TEST(test_storage_recommend_io_threads);
	MU_RUN_TEST(test_storage_describe);
	MU_RUN_TEST(test_prop_a_recommendation_is_never_zero_and_never_over_the_cap);
	MU_RUN_TEST(test_scan_bucket);
	MU_RUN_TEST(test_scan_workq_priority);
	MU_RUN_TEST(test_starved_worker_line_reads_idle);
	MU_RUN_TEST(test_scan_eta);
	MU_RUN_TEST(test_group_u64);
	MU_RUN_TEST(test_parse_size);
	MU_RUN_TEST(test_human_size);
	MU_RUN_TEST(test_human_duration);
	MU_RUN_TEST(test_num_digits);
	MU_RUN_TEST(test_longpath);
	MU_RUN_TEST(test_glob_basename);
	MU_RUN_TEST(test_glob_anchored_vs_any_depth);
	MU_RUN_TEST(test_glob_wildcards_respect_separators);
	MU_RUN_TEST(test_glob_double_star_without_a_separator);
	MU_RUN_TEST(test_glob_backslash_escapes);
	MU_RUN_TEST(test_glob_character_classes);
	MU_RUN_TEST(test_glob_directory_only);
	MU_RUN_TEST(test_glob_literal_paths_are_not_globs);
	MU_RUN_TEST(test_glob_reports_matching_pattern_and_counts);
	MU_RUN_TEST(test_glob_rejects_malformed);
	MU_RUN_TEST(test_glob_empty_set_matches_nothing);
	MU_RUN_TEST(test_prop_a_layout_matches_only_itself);
	MU_RUN_TEST(test_prop_a_split_record_is_not_the_layout_it_covers);
	MU_RUN_TEST(test_prop_stored_extents_come_back_where_they_were_put);
	MU_RUN_TEST(test_filerec_the_registry_finds_what_was_put_in_it);
	MU_RUN_TEST(test_filerec_is_unfindable_once_its_last_reference_goes);
	MU_RUN_TEST(test_prop_every_filerec_is_findable_by_its_own_id);
	MU_RUN_TEST(test_prop_the_hash_tree_counts_what_it_holds);
	MU_RUN_TEST(test_prop_removing_every_block_empties_the_hash_tree);
	MU_RUN_TEST(test_prop_sorting_puts_every_hash_head_in_offset_order);
	MU_RUN_TEST(test_prop_a_dup_group_counts_its_own_members);
	MU_RUN_TEST(test_removing_an_extent_collapses_a_group_of_one);
	MU_RUN_TEST(test_a_group_is_keyed_on_digest_and_length_together);
	MU_RUN_TEST(test_the_anchor_flag_latches_once_claimed);
	MU_RUN_TEST(test_insert_result_files_a_pair_under_one_length);
	MU_RUN_TEST(test_filerec_holds_one_descriptor_however_often_it_is_opened);
	MU_RUN_TEST(test_filerec_open_once_opens_each_file_exactly_once);
	MU_RUN_TEST(test_prop_a_filerec_token_is_found_by_its_filerec);
	MU_RUN_TEST(test_whole_file_dupes_load_at_poff_zero);
	MU_RUN_TEST(test_the_whole_file_target_prefers_readonly_then_fewest_extents);
	MU_RUN_TEST(test_every_window_elects_the_same_whole_file_target);
	MU_RUN_TEST(test_an_inlined_file_is_never_loaded_as_a_duplicate);
	MU_RUN_TEST(test_loading_one_filerec_treats_a_missing_id_as_success);
	MU_RUN_TEST(test_block_hashes_load_into_the_tree_in_offset_order);
	MU_RUN_TEST(test_extent_hashes_load_as_groups_carrying_their_offsets);
	MU_RUN_TEST(test_nondupe_extents_are_the_ones_nothing_else_shares);
	MU_RUN_TEST(test_nondupe_extents_grow_past_the_initial_capacity);
	MU_RUN_TEST(test_a_file_with_nothing_unique_yields_no_nondupe_extents);
	MU_RUN_TEST(test_a_whole_matching_run_is_recorded_as_one_group);
	MU_RUN_TEST(test_a_mismatch_splits_the_run_rather_than_ending_the_search);
	MU_RUN_TEST(test_each_side_of_a_match_records_its_own_offsets);
	MU_RUN_TEST(test_a_run_stops_where_the_blocks_stop_being_contiguous);
	MU_RUN_TEST(test_a_short_final_block_shortens_the_recorded_run);
	MU_RUN_TEST(test_the_extent_search_driven_by_its_pool);
	MU_RUN_TEST(test_the_interrupt_flag_and_its_test_hooks);
	MU_RUN_TEST(test_the_wind_down_notice_is_said_once);
	MU_RUN_TEST(test_fiemap_maps_a_real_file);
	MU_RUN_TEST(test_fiemap_range_answers_for_the_range_asked_for);
	MU_RUN_TEST(test_fiemap_counts_nothing_shared_in_a_fresh_file);
	MU_RUN_TEST(test_dedupe_classify_probe);

	/* The hashfile, against an in-memory SQLite - the same path a run
	 * without --hashfile takes, so none of this is a test-only seam. */
	MU_RUN_TEST(test_dbfile_files_are_unique_on_the_inode_pair);
	MU_RUN_TEST(test_dbfile_pruning_a_deleted_file_takes_its_hashes);
	MU_RUN_TEST(test_dbfile_advancing_the_generation_keeps_hashes_and_checkpoint);
	MU_RUN_TEST(test_dbfile_pruning_unscanned_files_spares_checkpointed_ones);
	MU_RUN_TEST(test_dbfile_scan_config_round_trips_and_coerces_the_old_auto);
	MU_RUN_TEST(test_dbfile_run_history_totals_accumulate_but_skips_are_the_last_run);
	MU_RUN_TEST(test_dbfile_layout_matches_compares_every_record);
	MU_RUN_TEST(test_dbfile_copying_a_donor_brings_all_three);
	MU_RUN_TEST(test_dbfile_a_stored_file_round_trips_every_field);
	MU_RUN_TEST(test_dbfile_hashes_round_trip_their_offsets);
	MU_RUN_TEST(test_dbfile_load_checkpoint_declines_what_it_cannot_vouch_for);
	MU_RUN_TEST(test_dbfile_pruning_keeps_what_still_exists);
	MU_RUN_TEST(test_dbfile_pruning_grows_past_its_initial_capacity);

	/* Property-based (src/proptest.h). Grouped rather than interleaved: they
	 * are a different question about the same code, and a failure here names
	 * a seed to replay rather than a case to read. */
	MU_RUN_TEST(test_prop_sanitize_ctrl_leaves_nothing_dangerous);
	MU_RUN_TEST(test_prop_sanitize_ctrl_truncates_on_whole_escapes);
	MU_RUN_TEST(test_prop_sanitize_ctrl_is_identity_on_ordinary_names);
	MU_RUN_TEST(test_prop_checksum_resumes_at_any_split);
	MU_RUN_TEST(test_prop_checksum_refuses_any_damaged_header);
	MU_RUN_TEST(test_prop_maps_share_with_themselves);
	MU_RUN_TEST(test_prop_maps_never_share_without_a_real_address);
	MU_RUN_TEST(test_prop_shared_ranges_have_nothing_left_to_free);
	MU_RUN_TEST(test_prop_unshared_bytes_are_bounded_and_credited_once);
	MU_RUN_TEST(test_prop_a_bare_pattern_reads_only_the_basename);
	MU_RUN_TEST(test_prop_adding_a_pattern_never_unexcludes_a_path);
	MU_RUN_TEST(test_prop_a_directory_pattern_never_matches_a_file);
	MU_RUN_TEST(test_prop_a_literal_path_matches_itself_and_nothing_else);
	MU_RUN_TEST(test_prop_parse_size_scales_by_the_suffix);
	MU_RUN_TEST(test_prop_human_size_picks_a_real_unit);
	MU_RUN_TEST(test_prop_human_duration_reads_back);
	MU_RUN_TEST(test_prop_num_digits_matches_printf);
	MU_RUN_TEST(test_prop_group_u64_truncates_to_a_prefix);
}

int main(int argc [[maybe_unused]], char *argv[]) {
	exec_path = argv[0];
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return MU_EXIT_CODE;
}
