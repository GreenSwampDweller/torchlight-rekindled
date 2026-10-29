#include "render/menu_chrome.h"

#include "core/log.h"

#include <OgreDataStream.h>
#include <OgreFont.h>
#include <OgreFontManager.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgreResourceGroupManager.h>
#include <OgreTechnique.h>
#include <OgreTextureUnitState.h>

#include <algorithm>
#include <map>
#include <string>

namespace tl::render {

namespace {
float g_ui_text_scale = 1.0f;
}

float ui_text_scale() { return g_ui_text_scale; }

void set_ui_text_scale(float scale) { g_ui_text_scale = std::clamp(scale, 0.8f, 1.4f); }

float ui_text_size_scale(int text_size) {
    constexpr float kTextSizes[] = {0.9f, 1.0f, 1.25f};
    return kTextSizes[std::clamp(text_size, 0, 2)];
}

Ogre::MaterialPtr build_menu_material(const std::string& name, const Ogre::TexturePtr& texture,
                                      float alpha) {
    auto& materials = Ogre::MaterialManager::getSingleton();
    if (Ogre::MaterialPtr existing = materials.getByName(name, Ogre::RGN_INTERNAL)) {
        materials.remove(existing);
    }
    Ogre::MaterialPtr material = materials.create(name, Ogre::RGN_INTERNAL);
    Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(false);
    pass->setDepthCheckEnabled(false);
    pass->setDepthWriteEnabled(false);
    pass->setCullingMode(Ogre::CULL_NONE);
    pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);

    Ogre::TextureUnitState* unit = pass->createTextureUnitState();
    unit->setTexture(texture);
    unit->setColourOperationEx(Ogre::LBX_SOURCE1, Ogre::LBS_TEXTURE, Ogre::LBS_CURRENT);
    if (alpha < 1.0f) {
        // A CEGUI window's `Alpha`, which multiplies everything it draws.
        unit->setAlphaOperation(Ogre::LBX_MODULATE, Ogre::LBS_TEXTURE, Ogre::LBS_MANUAL, 1.0f,
                                alpha);
    } else {
        unit->setAlphaOperation(Ogre::LBX_SOURCE1, Ogre::LBS_TEXTURE, Ogre::LBS_CURRENT);
    }
    // Clamped, because every panel here draws a sub-rectangle of an atlas and a
    // wrapping sampler bleeds the neighbouring image in along the seam.
    unit->setTextureAddressingMode(Ogre::TextureUnitState::TAM_CLAMP);
    return material;
}

float authored_height(const tl::formats::Imageset& reference, float fallback) {
    return reference.ok && reference.native_height > 0 ? static_cast<float>(reference.native_height)
                                                       : fallback;
}

namespace {
bool g_console_game_data = false;
std::vector<std::string> g_mod_imagesets;
}  // namespace

void set_mod_imagesets(std::vector<std::string> paths) { g_mod_imagesets = std::move(paths); }

void set_console_game_data(bool console) { g_console_game_data = console; }
bool console_game_data() { return g_console_game_data; }

// PORT: the PC sets a console-style screen falls back to.
std::vector<const char*> pc_imagesets() {
    std::vector<const char*> sets;
    if (!g_console_game_data) sets = {kLogoImageset, kGuiImageset, kUi2Imageset, kUiIconsImageset};
    for (const std::string& path : g_mod_imagesets) sets.push_back(path.c_str());
    return sets;
}

// PORT: the PC image in place of a console one. DIVERGENCE (data): the console screens drawn on
// a PC pak by the port's interface pack, in the PC's own art where it has the like.
std::string pc_image_for(const std::string& console_image) {
    static const std::map<std::string, std::string> kEquivalents = {
        {"bag", "ButtonInventory"},
        {"icon_defense", "IconDefense"},
        {"icon_dexterity", "IconRanged"},
        {"icon_magic", "IconMagic"},
        {"icon_strength", "IconMelee"},
        {"item_transmute_window", "ItemSlot"},
        {"pet_aggressive", "PetStanceAggressiveSelected"},
        {"pet_aggressive_inactive", "PetStanceAggressiveUnselected"},
        {"pet_defensive", "PetStanceDefensiveSelected"},
        {"pet_defensive_inactive", "PetStanceDefensiveUnselected"},
        {"pet_passive", "PetStancePassiveSelected"},
        {"pet_passive_inactive", "PetStancePassiveUnselected"},
        {"pethealth", "TargetHealthBar"},
        {"petmana", "PetManaBar"},
        {"petstance_aggressive", "PetStanceAggressiveSelected"},
        {"petstance_aggressive_inactive", "PetStanceAggressiveUnselected"},
        {"petstance_aggressive_rollover", "PetStanceAggressiveHover"},
        {"petstance_defensive", "PetStanceDefensiveSelected"},
        {"petstance_defensive_inactive", "PetStanceDefensiveUnselected"},
        {"petstance_defensive_rollover", "PetStanceDefensiveHover"},
        {"petstance_passive", "PetStancePassiveSelected"},
        {"petstance_passive_inactive", "PetStancePassiveUnselected"},
        {"petstance_passive_rollover", "PetStancePassiveHover"},
        {"plus", "SkillPointPlus"},
        {"plus_glow", "SkillPointPlusLit"},
        {"quest_button_incomplete", "ButtonStandardRed"},
        {"quest_button_rollover", "ButtonStandardRedHighlight"},
        {"skills_window_active", "ActiveSkillHousing"},
        {"skills_window_passive", "PassiveSkillHousing"},
        {"slider_bar", "SliderBar"},
        {"slider_bar_rollover", "SliderBar"},
        {"slider_tab", "sliderthumb"},
        {"slider_tab_rollover", "sliderthumb"},
        {"spells_window", "SkillSlot"},
        {"spells_windowb", "SkillSlot"},
        {"stat_icon_defense", "IconDefense"},
        {"stat_icon_dexterity", "IconRanged"},
        {"stat_icon_magic", "IconMagic"},
        {"stat_icon_strength", "IconMelee"},
        {"traycolor_blank", "ItemSlot"},
        {"traycolor_blue", "blueslotglow"},
        {"traycolor_gold", "goldslotglow"},
        {"traycolor_green", "greenslotglow"},
        {"traycolor_red", "redslotglow"},
        {"xbarfill", "PlayerXPBar"},
    };
    std::string key = console_image;
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    const auto found = kEquivalents.find(key);
    return found == kEquivalents.end() ? std::string() : found->second;
}

bool read_menu_resource(const std::string& path, const std::string& group,
                        std::vector<uint8_t>& bytes) {
    // The console-UI extraction's renamed sheet: beside an Xbox pak, absent as expected.
    if (g_console_game_data && path.find("xbox_itemicons13") != std::string::npos) return false;
    try {
        Ogre::DataStreamPtr stream =
            Ogre::ResourceGroupManager::getSingleton().openResource(path, group);
        if (!stream) return false;
        bytes.resize(static_cast<size_t>(stream->size()));
        if (!bytes.empty()) stream->read(bytes.data(), bytes.size());
        return true;
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("menu: could not read '%s': %s", path.c_str(), e.getDescription().c_str());
        return false;
    }
}

bool ensure_menu_font() {
    auto& fonts = Ogre::FontManager::getSingleton();
    if (fonts.getByName(kFontName, Ogre::RGN_INTERNAL)) return true;
    try {
        Ogre::FontPtr font = fonts.create(kFontName, Ogre::RGN_INTERNAL);
        font->setType(Ogre::FT_TRUETYPE);
        font->setSource(kFontFile);
        font->setTrueTypeSize(48);
        font->setTrueTypeResolution(96);
        font->addCodePointRange(Ogre::Font::CodePointRange(32, 255));
        font->load();
        return true;
    } catch (const Ogre::Exception& e) {
        TL_LOG_WARN("menu: could not build a font from '%s': %s", kFontFile,
                    e.getDescription().c_str());
        return false;
    }
}

float font_cell_per_em() {
    // The same TTF at the same size on every renderer this process builds, so
    // once is enough - and the value outlives a `Renderer::restart()` that
    // throws the font itself away.
    static float measured = 0.0f;
    if (measured > 0.0f) return measured;
    Ogre::FontPtr font = Ogre::FontManager::getSingleton().getByName(kFontName, Ogre::RGN_INTERNAL);
    if (!font || font->getTrueTypeSize() <= 0.0f || font->getTrueTypeResolution() == 0) {
        return 1.0f;
    }
    font->load();
    const Ogre::MaterialPtr& material = font->getMaterial();
    if (!material || material->getNumTechniques() == 0) return 1.0f;
    Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
    if (pass == nullptr || pass->getNumTextureUnitStates() == 0) return 1.0f;
    const Ogre::TexturePtr& atlas = pass->getTextureUnitState(0)->_getTexturePtr();
    if (!atlas || atlas->getHeight() == 0) return 1.0f;
    // Every glyph's rectangle is one cell tall; `M` is as good as any.
    const Ogre::Font::UVRect& uv = font->getGlyphTexCoords('M');
    const float cell = (uv.bottom - uv.top) * static_cast<float>(atlas->getHeight());
    const float em =
        font->getTrueTypeSize() * static_cast<float>(font->getTrueTypeResolution()) / 72.0f;
    if (!(cell > 0.0f) || !(em > 0.0f)) return 1.0f;
    measured = cell / em;
    TL_LOG_INFO("interface font: a glyph cell is %.3f em", static_cast<double>(measured));
    return measured;
}

}  // namespace tl::render
