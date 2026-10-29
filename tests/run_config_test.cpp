// core/run_config.h: `config.txt`, read over defaults and rewritten one value at a time.
#include "core/run_config.h"
#include "test_support.h"

#include <string>

using tl::core::parse_run_config;
using tl::core::run_config_text;
using tl::core::RunConfig;
using tl::core::set_run_config_value;

TL_TEST(a_new_config_file_reads_back_as_its_defaults) {
    const RunConfig defaults{false, false, false};
    const RunConfig read = parse_run_config(run_config_text(defaults), RunConfig{true, true, true});
    TL_CHECK(!read.debug);
    TL_CHECK(!read.logs);
    // Not written: the frame-rate overlay is a mod now.
    TL_CHECK(run_config_text(defaults).find("fps=") == std::string::npos);
}

TL_TEST(config_values_ignore_case_spaces_and_comments) {
    const RunConfig read = parse_run_config(
        "# a comment\n  DEBUG = 1\nlogs=0 # off\r\nFps=on\nunknown=1\n", RunConfig{});
    TL_CHECK(read.debug);
    TL_CHECK(!read.logs);
    TL_CHECK(read.fps);
}

TL_TEST(a_key_the_file_leaves_out_keeps_its_default) {
    const RunConfig read = parse_run_config("fps=1\n", RunConfig{true, true, false});
    TL_CHECK(read.debug);
    TL_CHECK(read.logs);
    TL_CHECK(read.fps);
}

TL_TEST(setting_a_value_rewrites_only_its_line) {
    const std::string text = "# keep me\ndebug=0\r\nfps=0 # mine\nlogs=1\n";
    TL_CHECK_EQ(set_run_config_value(text, "fps", true),
                std::string("# keep me\ndebug=0\r\nfps=1\nlogs=1\n"));
    TL_CHECK_EQ(set_run_config_value(text, "DEBUG", true),
                std::string("# keep me\ndebug=1\r\nfps=0 # mine\nlogs=1\n"));
}

TL_TEST(setting_a_missing_value_adds_its_line) {
    TL_CHECK_EQ(set_run_config_value("debug=1", "fps", true), std::string("debug=1\nfps=1\n"));
    TL_CHECK_EQ(set_run_config_value("", "fps", false), std::string("fps=0\n"));
}

TL_TEST(the_desktop_resolution_reads_as_numbers) {
    const RunConfig read =
        parse_run_config("width = 1920\nheight=1080\nfullscreen=1\n", RunConfig{});
    TL_CHECK_EQ(read.width, 1920u);
    TL_CHECK_EQ(read.height, 1080u);
    TL_CHECK(read.fullscreen);
    // Nonsense leaves the default.
    const RunConfig bad = parse_run_config("width=abc\nheight=-5\n", RunConfig{});
    TL_CHECK_EQ(bad.width, 0u);
    TL_CHECK_EQ(bad.height, 0u);
}

TL_TEST(only_a_desktop_file_carries_the_resolution) {
    const std::string device = run_config_text(RunConfig{});
    TL_CHECK(device.find("width=") == std::string::npos);
    RunConfig desktop;
    desktop.width = 1280;
    desktop.height = 720;
    const RunConfig read = parse_run_config(run_config_text(desktop), RunConfig{});
    TL_CHECK_EQ(read.width, 1280u);
    TL_CHECK_EQ(read.height, 720u);
    TL_CHECK(!read.fullscreen);
    TL_CHECK(read.vsync);
    TL_CHECK(!parse_run_config("vsync=0\n", RunConfig{}).vsync);
}

TL_TEST(the_frame_limit_is_in_every_file_and_reads_as_a_number) {
    RunConfig phone;
    phone.fps_limit = 60;
    const std::string text = run_config_text(phone);
    TL_CHECK(text.find("fps_limit=60") != std::string::npos);
    TL_CHECK_EQ(parse_run_config(text, RunConfig{}).fps_limit, 60u);
    // A file from before the key keeps the platform's default.
    TL_CHECK_EQ(parse_run_config("debug=0\n", phone).fps_limit, 60u);
    TL_CHECK_EQ(parse_run_config("fps_limit=0\n", phone).fps_limit, 0u);
    // Nonsense leaves the default.
    TL_CHECK_EQ(parse_run_config("fps_limit=-5\n", phone).fps_limit, 60u);
    TL_CHECK_EQ(parse_run_config("fps_limit=99999\n", phone).fps_limit, 60u);
}

/// The mods page's switch: written where the page is shown, on unless the file says 0.
TL_TEST(the_mods_menu_line_reads_back) {
    RunConfig off;
    off.mods_menu = false;
    const std::string text = run_config_text(off);
    TL_CHECK((text.find("mods_menu=0") != std::string::npos) == tl::core::kModsPage);
    TL_CHECK(parse_run_config("debug=1\n", RunConfig{}).mods_menu);
    TL_CHECK(!parse_run_config("mods_menu=0\n", RunConfig{}).mods_menu);
    TL_CHECK(parse_run_config("mods_menu = 1\n", off).mods_menu);
}

/// The free camera is a mod now: a new file has no line for it, and one from before is still read
/// (it says whether the mod starts on).
TL_TEST(the_free_camera_line_is_read_not_written) {
    RunConfig on;
    on.free_camera = true;
    const std::string text = run_config_text(on);
    TL_CHECK(text.find("free_camera") == std::string::npos);
    TL_CHECK(parse_run_config("free_camera=1\n", RunConfig{}).free_camera);
    TL_CHECK(!parse_run_config("free_camera=0\n", on).free_camera);
    // A file from before the key keeps the platform's default.
    TL_CHECK(parse_run_config("debug=1\n", on).free_camera);
    TL_CHECK(!parse_run_config("debug=1\n", RunConfig{}).free_camera);
}
