// The CEGUI XML reader (`formats/cegui.h`): attribute entities and colours.
#include "formats/cegui.h"
#include "test_support.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

std::string text_attribute(const std::string& xml) {
    const tl::formats::XmlDocument doc =
        tl::formats::parse_xml(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());
    TL_CHECK(doc.ok);
    return doc.root.attribute("Text", "");
}

}  // namespace

TL_TEST(xml_attributes_decode_the_named_entities) {
    TL_CHECK_EQ(text_attribute("<W Text=\"Save &amp; Quit\"/>"), std::string("Save & Quit"));
    TL_CHECK_EQ(text_attribute("<W Text=\"&lt;&gt;&quot;&apos;\"/>"), std::string("<>\"'"));
}

// `&#65;` and `&#x41;` are both 'A'. The hex form used to reach `strtol` as
// "x41", which base 16 does not read (it takes "0x", not a bare "x"), and came
// out as the raw text.
TL_TEST(xml_attributes_decode_decimal_and_hex_character_references) {
    TL_CHECK_EQ(text_attribute("<W Text=\"&#65;&#66;\"/>"), std::string("AB"));
    TL_CHECK_EQ(text_attribute("<W Text=\"&#x41;&#X42;\"/>"), std::string("AB"));
    // Outside ASCII, and not a number at all: passed through as written.
    TL_CHECK_EQ(text_attribute("<W Text=\"&#233;\"/>"), std::string("&#233;"));
    TL_CHECK_EQ(text_attribute("<W Text=\"&#xZZ;\"/>"), std::string("&#xZZ;"));
}

// CEGUI colours are AARRGGBB: 0x80 / 255 = 0.50196..., 0x40 / 255 = 0.25098...
TL_TEST(a_cegui_colour_is_alpha_first) {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
    TL_CHECK(tl::formats::parse_colour("80FF4000", r, g, b, a));
    TL_CHECK_EQ(a, 128.0f / 255.0f);
    TL_CHECK_EQ(r, 1.0f);
    TL_CHECK_EQ(g, 64.0f / 255.0f);
    TL_CHECK_EQ(b, 0.0f);
    TL_CHECK(!tl::formats::parse_colour("FF4000", r, g, b, a));
}

namespace {

template <typename T, typename F>
T parse_text(F parse, const std::string& xml) {
    return parse(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());
}

}  // namespace

// `hud.imageset` really carries `XPos="399 "`; a rectangle with no name or no
// area is dropped; names are found without case. Native resolution defaults
// to 640x480 when the file does not say.
TL_TEST(an_imageset_reads_its_rectangles) {
    const auto set = parse_text<tl::formats::Imageset>(
        tl::formats::parse_imageset,
        "<Imageset Name=\"HUD\" Imagefile=\"media/ui/hud.dds\" NativeHorzRes=\"1024\" "
        "NativeVertRes=\"768\" AutoScaled=\"true\">\n"
        "  <Image Name=\"ButtonQuests\" XPos=\"399 \" YPos=\"12\" Width=\"40\" Height=\"41\"/>\n"
        "  <Image Name=\"Empty\" XPos=\"0\" YPos=\"0\" Width=\"0\" Height=\"8\"/>\n"
        "</Imageset>\n");
    TL_CHECK(set.ok);
    TL_CHECK_EQ(set.name, std::string("HUD"));
    TL_CHECK_EQ(set.image_file, std::string("media/ui/hud.dds"));
    TL_CHECK_EQ(set.native_width, 1024);
    TL_CHECK_EQ(set.native_height, 768);
    TL_CHECK(set.auto_scaled);
    TL_CHECK_EQ(set.images.size(), size_t{1});
    const tl::formats::ImagesetImage* quests = set.find("buttonquests");
    TL_CHECK(quests != nullptr);
    TL_CHECK_EQ(quests->x, 399);
    TL_CHECK_EQ(quests->height, 41);
    TL_CHECK(set.find("Empty") == nullptr);

    const auto bare = parse_text<tl::formats::Imageset>(
        tl::formats::parse_imageset,
        "<Imageset><Image Name=\"A\" Width=\"1\" Height=\"1\"/></Imageset>");
    TL_CHECK(bare.ok);
    TL_CHECK_EQ(bare.native_width, 640);
    TL_CHECK_EQ(bare.native_height, 480);
    TL_CHECK(!parse_text<tl::formats::Imageset>(tl::formats::parse_imageset, "<Imageset/>").ok);
    TL_CHECK(!parse_text<tl::formats::Imageset>(tl::formats::parse_imageset, "<GUILayout/>").ok);
}

// A layout's one root window, its properties in order, nested windows, and a
// depth-first search by exact name.
TL_TEST(a_gui_layout_reads_its_window_tree) {
    const auto layout = parse_text<tl::formats::GuiLayout>(
        tl::formats::parse_gui_layout,
        "<?xml version=\"1.0\" ?>\n<GUILayout>\n"
        "<Window Type=\"DefaultWindow\" Name=\"Root\">\n"
        "  <Property Name=\"UnifiedAreaRect\" Value=\"{{0,0},{0,0},{1,0},{1,0}}\"/>\n"
        "  <Window Type=\"GuiLook/StandardButton\" Name=\"Root/Quit\">\n"
        "    <Property Name=\"Text\" Value=\"Quit\"/>\n"
        "  </Window>\n"
        "</Window>\n</GUILayout>\n");
    TL_CHECK(layout.ok);
    TL_CHECK_EQ(layout.root.name, std::string("Root"));
    TL_CHECK(layout.root.has_property("UnifiedAreaRect"));
    const tl::formats::GuiWindow* quit = layout.find("Root/Quit");
    TL_CHECK(quit != nullptr);
    TL_CHECK_EQ(quit->type, std::string("GuiLook/StandardButton"));
    TL_CHECK_EQ(quit->property("Text", "?"), std::string("Quit"));
    TL_CHECK_EQ(quit->property("Tooltip", "?"), std::string("?"));
    TL_CHECK(layout.find("root/quit") == nullptr);
    TL_CHECK(!parse_text<tl::formats::GuiLayout>(tl::formats::parse_gui_layout, "<GUILayout/>").ok);
}

// `{{0.5,-507},{1,-64}}`, whitespace as the files have it; 0.5 * 1024 - 507 * 1
// = 5, and with a pixel scale of 2, 0.5 * 1024 - 507 * 2 = -502.
TL_TEST(a_uvector2_reads_four_numbers_between_the_braces) {
    tl::formats::UVector2 v;
    TL_CHECK(tl::formats::parse_uvector2("{{0.5,-507},{ 1 , -64 }}", v));
    TL_CHECK_EQ(v.x.scale, 0.5f);
    TL_CHECK_EQ(v.x.offset, -507.0f);
    TL_CHECK_EQ(v.y.scale, 1.0f);
    TL_CHECK_EQ(v.y.offset, -64.0f);
    TL_CHECK_EQ(v.x.resolve(1024.0f, 1.0f), 5.0f);
    TL_CHECK_EQ(v.x.resolve(1024.0f, 2.0f), -502.0f);
    TL_CHECK(!tl::formats::parse_uvector2("{{0.5,-507},{1}}", v));
    TL_CHECK(!tl::formats::parse_uvector2("{{a,b},{c,d}}", v));
}

TL_TEST(an_image_reference_splits_set_and_image) {
    std::string set, image;
    TL_CHECK(tl::formats::parse_image_reference("set:Logo image:topbar", set, image));
    TL_CHECK_EQ(set, std::string("Logo"));
    TL_CHECK_EQ(image, std::string("topbar"));
    TL_CHECK(!tl::formats::parse_image_reference("", set, image));
    TL_CHECK(!tl::formats::parse_image_reference("image:topbar set:Logo", set, image));
    TL_CHECK(!tl::formats::parse_image_reference("set: image:topbar", set, image));
}

// The PC's layouts are UTF-16LE with a byte-order mark; they parse like the UTF-8 ones.
TL_TEST(cegui_xml_reads_utf16le_with_a_bom) {
    const std::string text = "<GUILayout><Window Type=\"A\" Name=\"B\" /></GUILayout>";
    std::vector<uint8_t> bytes = {0xff, 0xfe};
    for (const char c : text) {
        bytes.push_back(static_cast<uint8_t>(c));
        bytes.push_back(0);
    }
    const tl::formats::XmlDocument doc = tl::formats::parse_xml(bytes.data(), bytes.size());
    TL_CHECK(doc.ok);
    TL_CHECK_EQ(doc.root.name, std::string("GUILayout"));
    TL_CHECK_EQ(doc.root.children.size(), size_t{1});
    TL_CHECK_EQ(doc.root.children[0].attribute("Name", ""), std::string("B"));
}
