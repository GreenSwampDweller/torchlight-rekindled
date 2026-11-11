#include "formats/cegui.h"

#include "formats/byte_reader.h"

#include <cctype>
#include <cstdlib>

namespace tl::formats {
namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && is_space(text[begin])) ++begin;
    while (end > begin && is_space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool equal_ignoring_case(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

int to_int(const std::string& text, int fallback) {
    const std::string cleaned = trim(text);
    if (cleaned.empty()) return fallback;
    return std::atoi(cleaned.c_str());
}

/// Expand the five entities XML defines, and ASCII numeric references.
std::string unescape(const std::string& text) {
    if (text.find('&') == std::string::npos) return text;
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] != '&') {
            out.push_back(text[i++]);
            continue;
        }
        const size_t semicolon = text.find(';', i);
        if (semicolon == std::string::npos || semicolon - i > 8) {
            out.push_back(text[i++]);
            continue;
        }
        const std::string entity = text.substr(i + 1, semicolon - i - 1);
        if (entity == "amp") {
            out.push_back('&');
        } else if (entity == "lt") {
            out.push_back('<');
        } else if (entity == "gt") {
            out.push_back('>');
        } else if (entity == "quot") {
            out.push_back('"');
        } else if (entity == "apos") {
            out.push_back('\'');
        } else if (!entity.empty() && entity[0] == '#') {
            // Only ASCII is produced; anything else passes through unchanged.
            // `&#x41;`: skip the "x" ourselves, strtol's base 16 only skips "0x".
            const bool hex = entity.size() > 1 && lower(entity[1]) == 'x';
            const char* digits = entity.c_str() + (hex ? 2 : 1);
            char* end = nullptr;
            const long code = std::strtol(digits, &end, hex ? 16 : 10);
            if (end != digits && *end == '\0' && code > 0 && code < 128) {
                out.push_back(static_cast<char>(code));
            } else {
                out.append(text, i, semicolon - i + 1);
            }
        } else {
            out.append(text, i, semicolon - i + 1);
            i = semicolon + 1;
            continue;
        }
        i = semicolon + 1;
    }
    return out;
}

/// A minimal pull parser: elements and attributes only, no text, CDATA or namespaces.
class Parser {
public:
    Parser(const uint8_t* data, size_t size) : text_(reinterpret_cast<const char*>(data), size) {
        // Skip a UTF-8 BOM; the game's text files are inconsistently encoded.
        if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
            static_cast<unsigned char>(text_[1]) == 0xBB &&
            static_cast<unsigned char>(text_[2]) == 0xBF) {
            at_ = 3;
        }
    }

    bool parse(XmlElement& root, std::string& error) {
        std::vector<XmlElement*> stack;
        bool have_root = false;

        while (true) {
            const size_t open = text_.find('<', at_);
            if (open == std::string::npos) break;
            at_ = open + 1;
            if (at_ >= text_.size()) break;

            // <?xml ... ?> and <!-- ... --> and <!DOCTYPE ...>
            if (text_[at_] == '?') {
                const size_t end = text_.find("?>", at_);
                if (end == std::string::npos) break;
                at_ = end + 2;
                continue;
            }
            if (text_.compare(at_, 3, "!--") == 0) {
                const size_t end = text_.find("-->", at_);
                if (end == std::string::npos) break;
                at_ = end + 3;
                continue;
            }
            if (text_[at_] == '!') {
                const size_t end = text_.find('>', at_);
                if (end == std::string::npos) break;
                at_ = end + 1;
                continue;
            }

            // </Name>
            if (text_[at_] == '/') {
                const size_t end = text_.find('>', at_);
                if (end == std::string::npos) break;
                at_ = end + 1;
                if (stack.empty()) {
                    error = "closing tag with nothing open";
                    return false;
                }
                stack.pop_back();
                continue;
            }

            // <Name attr="value" ... > or <Name ... />
            XmlElement element;
            if (!read_element(element, error)) return false;
            const bool self_closing = self_closing_;

            XmlElement* placed = nullptr;
            if (stack.empty()) {
                if (have_root) {
                    // A second root is ignored, not an error.
                    break;
                }
                root = std::move(element);
                placed = &root;
                have_root = true;
            } else {
                stack.back()->children.push_back(std::move(element));
                placed = &stack.back()->children.back();
            }
            if (!self_closing) stack.push_back(placed);
        }

        if (!have_root) {
            error = "no elements";
            return false;
        }
        return true;
    }

private:
    bool read_element(XmlElement& element, std::string& error) {
        self_closing_ = false;
        const size_t name_begin = at_;
        while (at_ < text_.size() && !is_space(text_[at_]) && text_[at_] != '>' &&
               text_[at_] != '/') {
            ++at_;
        }
        element.name = text_.substr(name_begin, at_ - name_begin);
        if (element.name.empty()) {
            error = "empty tag name";
            return false;
        }

        while (at_ < text_.size()) {
            while (at_ < text_.size() && is_space(text_[at_])) ++at_;
            if (at_ >= text_.size()) break;
            if (text_[at_] == '>') {
                ++at_;
                return true;
            }
            if (text_[at_] == '/') {
                self_closing_ = true;
                ++at_;
                if (at_ < text_.size() && text_[at_] == '>') ++at_;
                return true;
            }

            const size_t key_begin = at_;
            while (at_ < text_.size() && text_[at_] != '=' && !is_space(text_[at_]) &&
                   text_[at_] != '>' && text_[at_] != '/') {
                ++at_;
            }
            const std::string key = text_.substr(key_begin, at_ - key_begin);
            while (at_ < text_.size() && is_space(text_[at_])) ++at_;
            if (at_ >= text_.size() || text_[at_] != '=') {
                // An attribute with no value is skipped.
                continue;
            }
            ++at_;
            while (at_ < text_.size() && is_space(text_[at_])) ++at_;
            if (at_ >= text_.size()) break;
            const char quote = text_[at_];
            if (quote != '"' && quote != '\'') {
                error = "unquoted attribute value in <" + element.name + ">";
                return false;
            }
            ++at_;
            const size_t value_begin = at_;
            while (at_ < text_.size() && text_[at_] != quote) ++at_;
            const std::string value = text_.substr(value_begin, at_ - value_begin);
            if (at_ < text_.size()) ++at_;
            element.attributes.emplace_back(key, unescape(value));
        }

        error = "unterminated tag <" + element.name + ">";
        return false;
    }

    std::string text_;
    size_t at_ = 0;
    bool self_closing_ = false;
};

void collect_windows(const XmlElement& element, GuiWindow& window) {
    window.type = element.attribute("Type", {});
    window.name = element.attribute("Name", {});
    for (const XmlElement& child : element.children) {
        if (equal_ignoring_case(child.name, "Property")) {
            window.properties.emplace_back(child.attribute("Name", {}),
                                           child.attribute("Value", {}));
        } else if (equal_ignoring_case(child.name, "Window")) {
            GuiWindow nested;
            collect_windows(child, nested);
            window.children.push_back(std::move(nested));
        }
    }
}

const GuiWindow* find_window(const GuiWindow& window, const std::string& name) {
    if (window.name == name) return &window;
    for (const GuiWindow& child : window.children) {
        if (const GuiWindow* found = find_window(child, name)) return found;
    }
    return nullptr;
}

}  // namespace

std::string XmlElement::attribute(const std::string& key, const std::string& fallback) const {
    for (const auto& entry : attributes) {
        if (equal_ignoring_case(entry.first, key)) return entry.second;
    }
    return fallback;
}

bool XmlElement::has_attribute(const std::string& key) const {
    for (const auto& entry : attributes) {
        if (equal_ignoring_case(entry.first, key)) return true;
    }
    return false;
}

XmlDocument parse_xml(const uint8_t* data, size_t size) {
    XmlDocument document;
    if (data == nullptr || size == 0) {
        document.error = "empty";
        return document;
    }
    // The PC's layouts are UTF-16LE with a byte-order mark; the console's exports are UTF-8.
    if (size >= 2 && data[0] == 0xff && data[1] == 0xfe) {
        const std::string text = utf16le_to_utf8(data + 2, size - 2);
        Parser parser(reinterpret_cast<const uint8_t*>(text.data()), text.size());
        document.ok = parser.parse(document.root, document.error);
        return document;
    }
    Parser parser(data, size);
    document.ok = parser.parse(document.root, document.error);
    return document;
}

const ImagesetImage* Imageset::find(const std::string& image_name) const {
    for (const ImagesetImage& image : images) {
        if (equal_ignoring_case(image.name, image_name)) return &image;
    }
    return nullptr;
}

Imageset parse_imageset(const uint8_t* data, size_t size) {
    Imageset set;
    const XmlDocument document = parse_xml(data, size);
    if (!document.ok) {
        set.error = document.error;
        return set;
    }
    if (!equal_ignoring_case(document.root.name, "Imageset")) {
        set.error = "root is <" + document.root.name + ">, not <Imageset>";
        return set;
    }

    const std::string empty;
    set.name = document.root.attribute("Name", empty);
    set.image_file = document.root.attribute("Imagefile", empty);
    set.native_width = to_int(document.root.attribute("NativeHorzRes", empty), 640);
    set.native_height = to_int(document.root.attribute("NativeVertRes", empty), 480);
    const std::string scaled = trim(document.root.attribute("AutoScaled", empty));
    set.auto_scaled = equal_ignoring_case(scaled, "true") || scaled == "1";
    const std::string tint = trim(document.root.attribute("Tint", empty));
    if (tint.size() == 6) set.tint = static_cast<uint32_t>(std::strtoul(tint.c_str(), nullptr, 16));

    for (const XmlElement& child : document.root.children) {
        if (!equal_ignoring_case(child.name, "Image")) continue;
        ImagesetImage image;
        image.name = child.attribute("Name", empty);
        // Trimmed: `hud.imageset` carries `XPos="399 "` on ButtonQuests.
        image.x = to_int(child.attribute("XPos", empty), 0);
        image.y = to_int(child.attribute("YPos", empty), 0);
        image.width = to_int(child.attribute("Width", empty), 0);
        image.height = to_int(child.attribute("Height", empty), 0);
        if (image.name.empty() || image.width == 0 || image.height == 0) continue;
        set.images.push_back(std::move(image));
    }

    if (set.images.empty()) {
        set.error = "no <Image> entries";
        return set;
    }
    set.ok = true;
    return set;
}

std::string GuiWindow::property(const std::string& key, const std::string& fallback) const {
    for (const auto& entry : properties) {
        if (equal_ignoring_case(entry.first, key)) return entry.second;
    }
    return fallback;
}

bool GuiWindow::has_property(const std::string& key) const {
    for (const auto& entry : properties) {
        if (equal_ignoring_case(entry.first, key)) return true;
    }
    return false;
}

const GuiWindow* GuiLayout::find(const std::string& window_name) const {
    return find_window(root, window_name);
}

GuiLayout parse_gui_layout(const uint8_t* data, size_t size) {
    GuiLayout layout;
    const XmlDocument document = parse_xml(data, size);
    if (!document.ok) {
        layout.error = document.error;
        return layout;
    }
    if (!equal_ignoring_case(document.root.name, "GUILayout")) {
        layout.error = "root is <" + document.root.name + ">, not <GUILayout>";
        return layout;
    }
    // <GUILayout> holds one <Window>, which is the tree's root.
    for (const XmlElement& child : document.root.children) {
        if (!equal_ignoring_case(child.name, "Window")) continue;
        collect_windows(child, layout.root);
        layout.ok = true;
        break;
    }
    if (!layout.ok) layout.error = "no <Window> under <GUILayout>";
    return layout;
}

bool parse_uvector2(const std::string& text, UVector2& out) {
    // Four numbers between the braces; the files' whitespace is inconsistent.
    float values[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    size_t found = 0;
    size_t at = 0;
    while (found < 4 && at < text.size()) {
        while (at < text.size() &&
               (text[at] == '{' || text[at] == '}' || text[at] == ',' || is_space(text[at]))) {
            ++at;
        }
        if (at >= text.size()) break;
        char* end = nullptr;
        const double value = std::strtod(text.c_str() + at, &end);
        if (end == text.c_str() + at) return false;
        values[found++] = static_cast<float>(value);
        at = static_cast<size_t>(end - text.c_str());
    }
    if (found != 4) return false;
    out.x.scale = values[0];
    out.x.offset = values[1];
    out.y.scale = values[2];
    out.y.offset = values[3];
    return true;
}

bool parse_image_reference(const std::string& text, std::string& set, std::string& image) {
    const size_t set_at = text.find("set:");
    const size_t image_at = text.find("image:");
    if (set_at == std::string::npos || image_at == std::string::npos || image_at < set_at) {
        return false;
    }
    set = trim(text.substr(set_at + 4, image_at - (set_at + 4)));
    image = trim(text.substr(image_at + 6));
    return !set.empty() && !image.empty();
}

bool parse_colour(const std::string& text, float& r, float& g, float& b, float& a) {
    const std::string hex = trim(text);
    if (hex.size() != 8) return false;
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    const unsigned long value = std::strtoul(hex.c_str(), nullptr, 16);
    a = static_cast<float>((value >> 24) & 0xFF) / 255.0f;
    r = static_cast<float>((value >> 16) & 0xFF) / 255.0f;
    g = static_cast<float>((value >> 8) & 0xFF) / 255.0f;
    b = static_cast<float>(value & 0xFF) / 255.0f;
    return true;
}

}  // namespace tl::formats
