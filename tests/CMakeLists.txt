# Desktop-only. The Switch build never reaches this file.
#
# Tests that need real game data skip gracefully when it is absent - a
# contributor without a copy of Torchlight must still get a green build.

add_executable(tl_tests
    test_main.cpp
    game_data.cpp
    byte_reader_test.cpp
    ogre_skeleton_test.cpp
    asset_path_test.cpp
    cmp_test.cpp
    uilayout_test.cpp
    cegui_test.cpp
    ogg_test.cpp
    float_convert_test.cpp
    adm_test.cpp
    dungeon_test.cpp
    collision_test.cpp
    script_test.cpp
    puzzle_input_test.cpp
    unit_trigger_test.cpp
    dialog_test.cpp
    quest_test.cpp
    markup_runs_test.cpp
    run_config_test.cpp
    mods_test.cpp
    sound_data_test.cpp
    layout_text_test.cpp
    output_incrementor_test.cpp
    cinematic_node_test.cpp
    menu_manager_test.cpp
    sound_object_test.cpp
    music_object_test.cpp
    quest_dialog_test.cpp
    navigation_test.cpp
    job_pool_test.cpp
    unit_observers_test.cpp
    unit_resource_test.cpp
    passability_test.cpp
    inventory_test.cpp
    wardrobe_test.cpp
    item_use_test.cpp
    character_test.cpp
    journal_test.cpp
    stats_page_test.cpp
    gamepad_test.cpp
    runic_math_test.cpp
    runic_strings_test.cpp
    targeting_test.cpp
    item_drop_test.cpp
    no_spawn_region_test.cpp
    populate_test.cpp
    item_label_test.cpp
    breakable_test.cpp
    unit_spawner_test.cpp
    new_game_menu_test.cpp
    difficulty_menu_test.cpp
    continue_menu_test.cpp
    interactive_menu_test.cpp
    skill_apply_test.cpp
    attack_effects_test.cpp
    model_animation_test.cpp
    equipment_attach_test.cpp
    layout_schema_test.cpp
    settings_menu_test.cpp
    text_event_test.cpp
    context_tips_test.cpp
    display_gamma_test.cpp
    howto_menu_test.cpp
    inventory_screen_test.cpp
    tray_press_test.cpp
    tray_row_test.cpp
    equipment_tooltip_test.cpp
    skills_test.cpp
    skills_tab_test.cpp
    skill_grid_test.cpp
    active_range_test.cpp
    light_override_test.cpp
    effects_test.cpp
    effect_manager_test.cpp
    skill_cast_test.cpp
    skill_animation_test.cpp
    attack_button_test.cpp
    attack_hands_test.cpp
    animation_manifest_test.cpp
    skill_text_test.cpp
    hotbar_test.cpp
    game_ui_test.cpp
    input_test.cpp
    pet_test.cpp
    idle_ai_types_test.cpp
    layout_inputs_test.cpp
    unit_skill_target_test.cpp
    sound_bank_test.cpp
    wav_test.cpp
    unit_data_test.cpp
    automap_grid_test.cpp
    touch_pad_test.cpp
    dds_test.cpp
    audio_mix_test.cpp
    level_memory_test.cpp
    town_portal_test.cpp
    waypoints_test.cpp
    warp_test.cpp
    death_test.cpp
    death_flight_test.cpp
    jump_down_test.cpp
    unit_types_test.cpp
    unit_book_test.cpp
    combat_test.cpp
    stats_test.cpp
    spawn_class_test.cpp
    item_roll_test.cpp
    timeline_test.cpp
    game_speed_test.cpp
    cinematics_test.cpp
    camera_shake_test.cpp
    weapon_trail_test.cpp
    perform_attack_test.cpp
    weapon_procs_test.cpp
    struck_procs_test.cpp
    transform_test.cpp
    ai_flags_test.cpp
    ai_skills_test.cpp
    ai_stat_watcher_test.cpp
    hp_events_test.cpp
    item_sets_test.cpp
    unit_themes_test.cpp
    item_ai_test.cpp
    item_effects_test.cpp
    combine_test.cpp
    merchant_test.cpp
    stash_test.cpp
    enchanting_test.cpp
    fishing_test.cpp
    item_save_test.cpp
    item_name_test.cpp
    missile_test.cpp
    retirement_test.cpp
    pet_commands_test.cpp
)
target_include_directories(tl_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(tl_tests PRIVATE tl_formats tl_core)
tl_set_warnings(tl_tests)

add_test(NAME unit COMMAND tl_tests)

# The readers under corrupted input (fuzz/fuzz_formats.cpp). The ctest run is a
# smoke test; run `tl_fuzz 1000000 <seed>` in a sanitizer build for the real one.
add_executable(tl_fuzz fuzz/fuzz_formats.cpp fuzz/fuzz_loaders.cpp)
target_include_directories(tl_fuzz PRIVATE ${CMAKE_SOURCE_DIR}/src)
# The loader targets draw their key names from the loaders' own source.
target_compile_definitions(tl_fuzz PRIVATE TL_FUZZ_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(tl_fuzz PRIVATE tl_formats tl_core)
tl_set_warnings(tl_fuzz)
add_test(NAME fuzz_smoke COMMAND tl_fuzz 2000 1)

# Hot-path timings on a synthetic level (bench/bench_core.cpp). Built, not run,
# by default: timings belong in a Release build on a quiet machine.
add_executable(tl_bench bench/bench_core.cpp)
target_include_directories(tl_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tl_bench PRIVATE tl_formats tl_core)
tl_set_warnings(tl_bench)
