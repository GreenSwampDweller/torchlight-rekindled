#include "core/run_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace tl::core {
namespace {

RunConfig g_config;
std::string g_path;

std::string trimmed_lower(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) == 0) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

// A line's key, lower-cased, or empty for a comment, a blank line or one without `=`.
std::string key_of(const std::string& line, std::string* value = nullptr) {
    const std::string content = line.substr(0, line.find('#'));
    const size_t equals = content.find('=');
    if (equals == std::string::npos) return {};
    if (value != nullptr) *value = trimmed_lower(content.substr(equals + 1));
    return trimmed_lower(content.substr(0, equals));
}

bool read_file(const std::string& path, std::string& text) {
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (in == nullptr) return false;
    char buffer[1024];
    for (;;) {
        const size_t got = std::fread(buffer, 1, sizeof buffer, in);
        text.append(buffer, got);
        if (got < sizeof buffer) break;  // the end, or an error: nothing more to read
    }
    std::fclose(in);
    return true;
}

bool write_file(const std::string& path, const std::string& text) {
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (out == nullptr) return false;
    const bool written = std::fwrite(text.data(), 1, text.size(), out) == text.size();
    return std::fclose(out) == 0 && written;
}

}  // namespace

std::string run_config_text(const RunConfig& defaults) {
    std::ostringstream out;
    out << "# The port's own switches (0 off, 1 on).\n"
        << "# debug: the debug menu, bestiary, wardrobe, Choose Level, frame dumps and logs.\n"
        << "# logs:  the log file on its own.\n"
        << "debug=" << (defaults.debug ? 1 : 0) << "\n"
        << "logs=" << (defaults.logs ? 1 : 0) << "\n"
        << "# fps_limit: the most frames a second the game draws; 0 for no limit.\n"
        << "fps_limit=" << defaults.fps_limit << "\n";
    if (kModsPage) {
        out << "# mods_menu: 1 shows the mods page before the game; 0 skips it and starts\n"
            << "# without any mods.\n"
            << "mods_menu=" << (defaults.mods_menu ? 1 : 0) << "\n";
    }
    if (defaults.width > 0 && defaults.height > 0) {
        out << "# width, height: the window's size, or the screen's resolution in fullscreen.\n"
            << "# fullscreen: 1 fills the screen at that resolution.\n"
            << "# vsync: 1 waits for the screen's refresh; 0 draws frames as fast as it can.\n"
            << "width=" << defaults.width << "\n"
            << "height=" << defaults.height << "\n"
            << "fullscreen=" << (defaults.fullscreen ? 1 : 0) << "\n"
            << "vsync=" << (defaults.vsync ? 1 : 0) << "\n";
    }
    return out.str();
}

RunConfig parse_run_config(const std::string& text, const RunConfig& defaults) {
    RunConfig config = defaults;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::string value;
        const std::string key = key_of(line, &value);
        if (key.empty() || value.empty()) continue;
        const bool on = value != "0" && value != "off" && value != "false" && value != "no";
        if (key == "debug") config.debug = on;
        if (key == "logs") config.logs = on;
        if (key == "fps") config.fps = on;
        if (key == "fullscreen") config.fullscreen = on;
        if (key == "vsync") config.vsync = on;
        if (key == "free_camera") config.free_camera = on;
        if (key == "mods_menu") config.mods_menu = on;
        if (key == "fps_limit") {
            const long number = std::strtol(value.c_str(), nullptr, 10);
            if (number >= 0 && number <= 1000) config.fps_limit = static_cast<unsigned>(number);
        }
        if (key == "width" || key == "height") {
            const long number = std::strtol(value.c_str(), nullptr, 10);
            const unsigned size = number > 0 && number <= 16384 ? static_cast<unsigned>(number) : 0;
            if (size > 0) (key == "width" ? config.width : config.height) = size;
        }
    }
    return config;
}

std::string set_run_config_value(const std::string& text, const std::string& key, bool value) {
    const std::string wanted = trimmed_lower(key);
    const std::string replacement = wanted + "=" + (value ? "1" : "0");
    std::string out;
    bool found = false;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        const bool last = end == std::string::npos;
        if (last) end = text.size();
        std::string line = text.substr(start, end - start);
        const bool cr = !line.empty() && line.back() == '\r';
        if (!found && key_of(line) == wanted) {
            line = replacement + (cr ? "\r" : "");
            found = true;
        }
        out += line;
        if (!last) out += '\n';
        start = end + 1;
    }
    if (!found) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += replacement + "\n";
    }
    return out;
}

RunConfig load_run_config(const std::string& path, const RunConfig& defaults, bool create) {
    // Kept only where there is a file to keep it in: one that was there, or one
    // made now. The desktop, which makes none, saves nothing.
    g_path.clear();
    std::string text;
    if (read_file(path, text)) {
        g_config = parse_run_config(text, defaults);
        g_path = path;
    } else {
        g_config = defaults;
        if (create && write_file(path, run_config_text(defaults))) g_path = path;
    }
    return g_config;
}

bool save_run_config_value(const std::string& key, bool value) {
    const std::string wanted = trimmed_lower(key);
    if (wanted == "debug") g_config.debug = value;
    if (wanted == "logs") g_config.logs = value;
    if (wanted == "fps") g_config.fps = value;
    if (wanted == "mods_menu") g_config.mods_menu = value;
    if (g_path.empty()) return false;
    std::string text;
    if (!read_file(g_path, text)) text = run_config_text(g_config);
    return write_file(g_path, set_run_config_value(text, key, value));
}

const RunConfig& run_config() { return g_config; }

}  // namespace tl::core
