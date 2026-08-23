/*
 * The oans C unit suite: the runner, and nothing else.
 *
 * One translation unit per subject now, so this file holds only what must
 * exist exactly once - the MU_RUN_TEST list, main(), and the state minunit and
 * proptest keep. The two *_MAIN macros below are what pick this TU to own that
 * state; defining them anywhere else is a duplicate-symbol link error, and
 * defining them nowhere leaves the counters undefined at link time. Both loud,
 * which is the point: per-TU counters would give a suite that runs every test,
 * passes every assertion, and reports "0 tests" while exiting 0.
 *
 * The run order below is the order it has always been, and is deliberately not
 * grouped to match the file layout: every memdb() handle opens the same
 * shared-cache in-memory database, so several dbfile tests depend on running
 * before anything has stored a row.
 */
#include "suite.h"

/*
 * The state minunit and proptest declare `extern`, defined here because this is
 * the one file that exists exactly once. Their headers say why.
 */
int minunit_run = 0;
int minunit_assert = 0;
int minunit_fail = 0;
int minunit_status = 0;
double minunit_real_timer = 0;
double minunit_proc_timer = 0;
char minunit_last_message[MINUNIT_MESSAGE_LEN];
void (*minunit_setup)(void) = NULL;
void (*minunit_teardown)(void) = NULL;
uint64_t prop_seed_value;
bool prop_seed_resolved;

unsigned int blocksize = DEFAULT_BLOCKSIZE;
char *exec_path;

MU_TEST_SUITE(test_suite) {
	MU_RUN(test_running_checksum_survives_save_restore);
	MU_RUN(test_running_checksum_repoints_the_secret_on_restore);
	MU_RUN(test_running_checksum_save_refuses_a_short_buffer);
	MU_RUN(test_running_checksum_rejects_foreign_state);
	MU_RUN(test_is_block_zeroed);
	MU_RUN(test_block_len);
	MU_RUN(test_is_file_renamed);
	MU_RUN(test_seen_inode);
	MU_RUN(test_get_extent);
	MU_RUN(test_fiemap_layout_key);
	MU_RUN(test_fiemap_maps_share);
	MU_RUN(test_fiemap_unshared_bytes);
	MU_RUN(test_fiemap_unshared_bytes_accumulates);
	MU_RUN(test_fiemap_phys_set_grows);
	MU_RUN(test_sanitize_ctrl);
	MU_RUN(test_progress_copy_path);
	MU_RUN(test_progress_path_two_stage_render);
	MU_RUN(test_storage_recommend_io_threads);
	MU_RUN(test_storage_describe);
	MU_RUN(test_prop_a_recommendation_is_never_zero_and_never_over_the_cap);
	MU_RUN(test_scan_bucket);
	MU_RUN(test_scan_workq_priority);
	MU_RUN(test_starved_worker_line_reads_idle);
	MU_RUN(test_scan_eta);
	MU_RUN(test_group_u64);
	MU_RUN(test_parse_size);
	MU_RUN(test_human_size);
	MU_RUN(test_human_duration);
	MU_RUN(test_num_digits);
	MU_RUN(test_longpath);
	MU_RUN(test_prop_a_chunk_ends_on_a_boundary_and_is_the_longest_that_fits);
	MU_RUN(test_prop_the_chunk_walk_always_advances);
	MU_RUN(test_glob_basename);
	MU_RUN(test_glob_anchored_vs_any_depth);
	MU_RUN(test_glob_wildcards_respect_separators);
	MU_RUN(test_glob_double_star_without_a_separator);
	MU_RUN(test_glob_backslash_escapes);
	MU_RUN(test_glob_character_classes);
	MU_RUN(test_glob_directory_only);
	MU_RUN(test_glob_literal_paths_are_not_globs);
	MU_RUN(test_glob_reports_matching_pattern_and_counts);
	MU_RUN(test_glob_rejects_malformed);
	MU_RUN(test_glob_empty_set_matches_nothing);
	MU_RUN(test_prop_a_layout_matches_only_itself);
	MU_RUN(test_prop_a_split_record_is_not_the_layout_it_covers);
	MU_RUN(test_prop_stored_extents_come_back_where_they_were_put);
	MU_RUN(test_filerec_the_registry_finds_what_was_put_in_it);
	MU_RUN(test_filerec_is_unfindable_once_its_last_reference_goes);
	MU_RUN(test_prop_every_filerec_is_findable_by_its_own_id);
	MU_RUN(test_prop_the_hash_tree_counts_what_it_holds);
	MU_RUN(test_prop_removing_every_block_empties_the_hash_tree);
	MU_RUN(test_prop_sorting_puts_every_hash_head_in_offset_order);
	MU_RUN(test_prop_a_dup_group_counts_its_own_members);
	MU_RUN(test_removing_an_extent_collapses_a_group_of_one);
	MU_RUN(test_a_group_is_keyed_on_digest_and_length_together);
	MU_RUN(test_the_anchor_flag_latches_once_claimed);
	MU_RUN(test_insert_result_files_a_pair_under_one_length);
	MU_RUN(test_filerec_holds_one_descriptor_however_often_it_is_opened);
	MU_RUN(test_filerec_open_once_opens_each_file_exactly_once);
	MU_RUN(test_prop_a_filerec_token_is_found_by_its_filerec);
	MU_RUN(test_whole_file_dupes_load_at_poff_zero);
	MU_RUN(test_the_whole_file_target_prefers_readonly_then_fewest_extents);
	MU_RUN(test_every_window_elects_the_same_whole_file_target);
	MU_RUN(test_an_inlined_file_is_never_loaded_as_a_duplicate);
	MU_RUN(test_loading_one_filerec_treats_a_missing_id_as_success);
	MU_RUN(test_block_hashes_load_into_the_tree_in_offset_order);
	MU_RUN(test_extent_hashes_load_as_groups_carrying_their_offsets);
	MU_RUN(test_nondupe_extents_are_the_ones_nothing_else_shares);
	MU_RUN(test_nondupe_extents_grow_past_the_initial_capacity);
	MU_RUN(test_a_file_with_nothing_unique_yields_no_nondupe_extents);
	MU_RUN(test_a_whole_matching_run_is_recorded_as_one_group);
	MU_RUN(test_a_mismatch_splits_the_run_rather_than_ending_the_search);
	MU_RUN(test_each_side_of_a_match_records_its_own_offsets);
	MU_RUN(test_a_run_stops_where_the_blocks_stop_being_contiguous);
	MU_RUN(test_a_short_final_block_shortens_the_recorded_run);
	MU_RUN(test_the_extent_search_driven_by_its_pool);
	MU_RUN(test_the_interrupt_flag_and_its_test_hooks);
	MU_RUN(test_the_wind_down_notice_is_said_once);
	MU_RUN(test_fiemap_maps_a_real_file);
	MU_RUN(test_fiemap_range_answers_for_the_range_asked_for);
	MU_RUN(test_fiemap_counts_nothing_shared_in_a_fresh_file);
	MU_RUN(test_dedupe_classify_probe);

	/* The hashfile, against an in-memory SQLite - the same path a run
	 * without --hashfile takes, so none of this is a test-only seam. */
	MU_RUN(test_dbfile_files_are_unique_on_the_inode_pair);
	MU_RUN(test_dbfile_pruning_a_deleted_file_takes_its_hashes);
	MU_RUN(test_dbfile_advancing_the_generation_keeps_hashes_and_checkpoint);
	MU_RUN(test_dbfile_pruning_unscanned_files_spares_checkpointed_ones);
	MU_RUN(test_dbfile_scan_config_round_trips_and_coerces_the_old_auto);
	MU_RUN(test_dbfile_run_history_totals_accumulate_but_skips_are_the_last_run);
	MU_RUN(test_dbfile_layout_matches_compares_every_record);
	MU_RUN(test_dbfile_copying_a_donor_brings_all_three);
	MU_RUN(test_dbfile_a_stored_file_round_trips_every_field);
	MU_RUN(test_dbfile_hashes_round_trip_their_offsets);
	MU_RUN(test_dbfile_load_checkpoint_declines_what_it_cannot_vouch_for);
	MU_RUN(test_dbfile_pruning_keeps_what_still_exists);
	MU_RUN(test_dbfile_pruning_grows_past_its_initial_capacity);

	/* Property-based (src/proptest.h). Grouped rather than interleaved: they
	 * are a different question about the same code, and a failure here names
	 * a seed to replay rather than a case to read. */
	MU_RUN(test_prop_sanitize_ctrl_leaves_nothing_dangerous);
	MU_RUN(test_prop_sanitize_ctrl_truncates_on_whole_escapes);
	MU_RUN(test_prop_sanitize_ctrl_is_identity_on_ordinary_names);
	MU_RUN(test_prop_checksum_resumes_at_any_split);
	MU_RUN(test_prop_checksum_refuses_any_damaged_header);
	MU_RUN(test_prop_maps_share_with_themselves);
	MU_RUN(test_prop_maps_never_share_without_a_real_address);
	MU_RUN(test_prop_shared_ranges_have_nothing_left_to_free);
	MU_RUN(test_prop_unshared_bytes_are_bounded_and_credited_once);
	MU_RUN(test_prop_a_bare_pattern_reads_only_the_basename);
	MU_RUN(test_prop_adding_a_pattern_never_unexcludes_a_path);
	MU_RUN(test_prop_a_directory_pattern_never_matches_a_file);
	MU_RUN(test_prop_a_literal_path_matches_itself_and_nothing_else);
	MU_RUN(test_prop_parse_size_scales_by_the_suffix);
	MU_RUN(test_prop_human_size_picks_a_real_unit);
	MU_RUN(test_prop_human_duration_reads_back);
	MU_RUN(test_prop_num_digits_matches_printf);
	MU_RUN(test_prop_group_u64_truncates_to_a_prefix);
}

int main(int argc [[maybe_unused]], char *argv[]) {
	exec_path = argv[0];
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return MU_EXIT_CODE;
}
