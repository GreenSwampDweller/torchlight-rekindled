// The mouse layer: what the pointer is over, and where it points on the ground.
#include "render/mouse_game.h"

namespace tl::render {

// --- The ground point ------------------------------------------------------------------------

/// The floor the pointer is really on, as a rise from the player's feet: the ray cast
/// against the level, and a face to stand on sought under what it meets.
bool Renderer::Impl::MouseGame::floor_rise(const Vec3& origin, const Vec3& direction, float feet,
                                           float& rise) const {
    const float far = tl::core::kPointerRayLength;
    const Vec3 end{direction.x * far + origin.x, direction.y * far + origin.y,
                   direction.z * far + origin.z};
    tl::core::CollisionHit hit;
    if (!g.collision.ray_collision(origin, end, true, hit)) return false;
    if (hit.material == tl::core::kNoCollideMaterial) return false;
    const float dx = hit.point.x - origin.x;
    const float dy = hit.point.y - origin.y;
    const float dz = hit.point.z - origin.z;
    if (!(tl::core::kPointerNearHit < std::sqrt(dx * dx + dy * dy + dz * dz))) return false;
    const float probe = tl::core::kPointerFloorProbe;
    tl::core::CollisionHit floor;
    if (!g.collision.ray_collision({hit.point.x, hit.point.y + probe, hit.point.z},
                                   {hit.point.x, hit.point.y - probe, hit.point.z}, true, floor)) {
        return false;
    }
    if (floor.material == tl::core::kNoCollideMaterial) return false;
    if (!(tl::core::kPointerFloorNormal < floor.normal.y)) return false;
    rise = floor.point.y - feet;
    return std::fabs(rise) < tl::core::kPointerFloorReach;
}

// CGameClient::findWorldLocation
bool Renderer::Impl::MouseGame::world_location(bool raised, Vec3& out) {
    const Vec3 me = g.player.position();
    out = me;
    if (g.camera == nullptr || !input.pointer) return false;
    const Ogre::Ray ray = g.camera->getCameraToViewportRay(input.pointer_x, input.pointer_y);
    const Vec3 origin = to_core(ray.getOrigin());
    const Vec3 direction = to_core(ray.getDirection());
    const float raise = raised ? tl::core::kPointerRaise : 0.0f;
    out = tl::core::pointer_on_plane(origin, direction, me.y + raise);
    // A character standing still takes the height of the floor the pointer is on.
    float rise = 0.0f;
    if (!g.collision.empty() && !g.player.following() &&
        floor_rise(origin, direction, me.y, rise)) {
        out = tl::core::pointer_on_plane(origin, direction, me.y + rise + raise);
    }
    // `CGameUI::mouseOverPanel`: over the interface it points at nothing.
    return !hud_blocks;
}

// --- Items by handle -------------------------------------------------------------------------

Unit Renderer::Impl::MouseGame::item_handle(const Item& item) {
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& have = items[i];
        if (have.breakable == item.breakable && have.target.kind == item.target.kind &&
            have.target.dropped == item.target.dropped &&
            have.target.machine == item.target.machine && have.target.node == item.target.node &&
            have.target.portal == item.target.portal) {
            return i + 1;
        }
    }
    items.push_back(item);
    return items.size();
}

auto Renderer::Impl::MouseGame::item_of(Unit handle) const -> const Item* {
    if (handle == 0 || handle > items.size()) return nullptr;
    return &items[static_cast<size_t>(handle - 1)];
}

/// Where a prop, a sleeping mimic or a portal is, or false when it is gone.
bool Renderer::Impl::MouseGame::prop_position(const PlayerItemTarget& target, Vec3& at) const {
    if (target.kind == Kind::Portal) {
        for (const OpenPortal& portal : g.open_portals) {
            if (portal.unit != target.portal || !portal.enabled) continue;
            at = {portal.at.x, portal.at.y, portal.at.z};
            return true;
        }
        return false;
    }
    tl::core::LevelScript* owner = g.machine_script(target.machine);
    if (owner == nullptr) return false;
    if (target.kind == Kind::Mimic) {
        return owner->unit_position(target.node, at) && owner->unit_mimic_idle(target.node);
    }
    for (const tl::core::LevelScript::UnitTriggerView& view : owner->unit_triggers()) {
        if (view.id != target.node) continue;
        at = view.at;
        return view.armed;
    }
    return false;
}

/// Where a mouse item is, or false when the level no longer holds it.
bool Renderer::Impl::MouseGame::item_position(Unit handle, Vec3& at) const {
    const Item* item = item_of(handle);
    if (item == nullptr) return false;
    if (item->breakable != 0) {
        const int index = g.breakable_index(item->breakable);
        if (index < 0) return false;
        const BreakableWorld& b = g.breakables[static_cast<size_t>(index)];
        if (!b.state.usable || b.node == nullptr) return false;
        at = to_core(b.node->_getDerivedPosition());
        return true;
    }
    if (item->target.kind == Kind::None) return false;
    if (item->target.kind != Kind::Dropped) return prop_position(item->target, at);
    for (const DroppedWorldItem& dropped : g.dropped_items) {
        if (dropped.node != item->target.dropped) continue;
        at = to_core(dropped.node->getPosition());
        return true;
    }
    return false;
}

/// A character's unit name by its node, among the level's units.
std::string Renderer::Impl::MouseGame::unit_name(int64_t node) const {
    if (node == 0) return {};
    for (const auto& entry : g.scene_units) {
        if (entry.second.node == node) return entry.first;
    }
    for (const auto& owner : g.spawned) {
        for (const SpawnedUnit& made : owner.second) {
            if (made.id == node) return made.unit;
        }
    }
    return {};
}

std::string Renderer::Impl::MouseGame::name_of(Unit character) const {
    for (const Character& unit : units) {
        if (static_cast<Unit>(unit.node) == character) return unit.name;
    }
    return unit_name(static_cast<int64_t>(character));
}

// --- The picks -------------------------------------------------------------------------------

tl::core::ScreenMatrix Renderer::Impl::MouseGame::screen_matrix() const {
    float projection[16];
    float view[16];
    const Ogre::Matrix4& p = g.camera->getProjectionMatrix();
    const Ogre::Affine3& v = g.camera->getViewMatrix();
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            projection[r * 4 + c] = p[r][c];
            view[r * 4 + c] = v[r][c];
        }
    }
    return tl::core::screen_matrix(projection, view);
}

// CLevel::findCharacterAtScreenCoordinates, the candidates: alive, active,
// targetable, shown and enabled, and neither the player nor a pet of the player's.
void Renderer::Impl::MouseGame::pick_character(const tl::core::ScreenMatrix& matrix, float x,
                                               float y, Over& over) {
    std::vector<tl::core::PickCandidate> candidates;
    const auto consider = [&](Ogre::Entity* entity, const std::string& name, int64_t node) {
        if (entity == nullptr || !entity->isVisible() || name.empty() || node == 0) return;
        const Ogre::SceneNode* at = entity->getParentSceneNode();
        if (at == nullptr || !g.node_targetable(node, at) || g.node_is_player_pet(node)) return;
        tl::core::LevelScript* owner = g.script_owning(node);
        if (owner == nullptr || owner->unit_is_dead(node)) return;
        const Vec3 position = to_core(at->_getDerivedPosition());
        if (!tl::core::pick_on_screen(matrix, position)) return;
        Vec3 low, high;
        entity_bounds(entity, low, high);
        tl::core::PickCandidate candidate;
        candidate.box = tl::core::character_screen_box(matrix, position, low, high);
        const int32_t side = owner->unit_console_alignment(node);
        candidate.alignment = side < 0 ? 2 : side;
        candidate.id = units.size();
        candidates.push_back(candidate);
        units.push_back({node, name, entity});
    };
    for (const auto& entry : g.scene_units) {
        consider(entry.second.entity, entry.first, entry.second.node);
    }
    for (const auto& owner : g.spawned) {
        for (const SpawnedUnit& made : owner.second) consider(made.entity, made.unit, made.id);
    }
    std::uint64_t picked = 0;
    if (!tl::core::pick_at(candidates.data(), candidates.size(), x, y, &picked)) return;
    const Character& found = units[static_cast<size_t>(picked)];
    over.unit_entity = found.entity;
    over.unit_name = found.name;
    over.unit_node = found.node;
    const tl::core::UnitStats* stats = g.unit_book.find(found.name);
    over.unit_hostile = stats != nullptr && g.unit_hostile(found.node, *stats);
}

void Renderer::Impl::MouseGame::add_item(const tl::core::ScreenMatrix& matrix, const Vec3& position,
                                         Ogre::Entity* entity, const Item& item,
                                         ItemPicks& picks) const {
    if (!tl::core::pick_on_screen(matrix, position)) return;
    Vec3 low, high;
    entity_bounds(entity, low, high);
    tl::core::PickCandidate candidate;
    candidate.box = tl::core::item_screen_box(matrix, position, low, high);
    candidate.id = picks.candidates.size();
    picks.candidates.push_back(candidate);
    picks.items.push_back(item);
    picks.entities.push_back(entity);
}

/// One machine's props and sleeping mimics.
void Renderer::Impl::MouseGame::add_props(const tl::core::ScreenMatrix& matrix, int machine,
                                          const tl::core::LevelScript& owner, const NodeMap& nodes,
                                          ItemPicks& picks) const {
    for (const tl::core::LevelScript::UnitTriggerView& prop : owner.unit_triggers()) {
        if (!prop.armed || g.non_selectable_prop(prop.unit)) continue;
        const auto presence = g.trigger_presence.find(g.instance_key(prop.id, machine));
        if (presence != g.trigger_presence.end() &&
            (!presence->second.presence.active || !presence->second.presence.visible)) {
            continue;
        }
        Item item;
        item.target.kind = Kind::Trigger;
        item.target.machine = machine;
        item.target.node = prop.id;
        add_item(matrix, prop.at, g.trigger_entity(prop.id, prop.unit, nodes), item, picks);
    }
    for (const tl::core::LevelScript::UnitTriggerView& mimic : owner.idle_mimics()) {
        Item item;
        item.target.kind = Kind::Mimic;
        item.target.machine = machine;
        item.target.node = mimic.id;
        Ogre::Entity* entity = nullptr;
        for (const auto& entry : g.scene_units) {
            if (entry.second.node == mimic.id) entity = entry.second.entity;
        }
        add_item(matrix, mimic.at, entity, item, picks);
    }
}

// CLevel::findItemAtScreenCoordinates, the candidates: usable, selectable
// and active.
void Renderer::Impl::MouseGame::pick_item(const tl::core::ScreenMatrix& matrix, float x, float y,
                                          Over& over) {
    ItemPicks picks;
    for (const DroppedWorldItem& dropped : g.dropped_items) {
        if (dropped.flying || !dropped.presence.active || !dropped.presence.visible) continue;
        Item item;
        item.target.kind = Kind::Dropped;
        item.target.dropped = dropped.node;
        add_item(matrix, to_core(dropped.node->getPosition()), dropped.entity, item, picks);
    }
    for (const BreakableWorld& b : g.breakables) {
        if (!b.state.usable || b.node == nullptr || !b.presence.active) continue;
        Item item;
        item.breakable = b.id;
        add_item(matrix, to_core(b.node->_getDerivedPosition()), b.entity, item, picks);
    }
    add_props(matrix, kLevelMachine, g.script, g.scripted_nodes, picks);
    for (size_t i = 0; i < g.nested_scripts.size(); ++i) {
        add_props(matrix, static_cast<int>(i), g.nested_scripts[i].script,
                  g.nested_scripts[i].nodes, picks);
    }
    for (const OpenPortal& portal : g.open_portals) {
        if (!portal.enabled) continue;
        Item item;
        item.target.kind = Kind::Portal;
        item.target.portal = portal.unit;
        item.target.portal_at = to_ogre(portal.at);
        add_item(matrix, {portal.at.x, portal.at.y, portal.at.z}, nullptr, item, picks);
    }
    std::uint64_t which = 0;
    if (!tl::core::pick_at(picks.candidates.data(), picks.candidates.size(), x, y, &which)) return;
    over.have_item = true;
    over.item = picks.items[static_cast<size_t>(which)];
    over.item_entity = picks.entities[static_cast<size_t>(which)];
}

/// The glow follows the pointer, as the pad's follows its selector.
void Renderer::Impl::MouseGame::show_character(const Over& over) {
    if (over.unit_entity != g.pad_highlighted) {
        if (g.pad_highlighted != nullptr && g.entity_still_placed(g.pad_highlighted)) {
            highlight_entity(g.pad_highlighted, false, g.pad_highlighted_hostile,
                             g.rim_light_texture);
        }
        const tl::core::UnitStats* picked =
            over.unit_entity != nullptr ? g.unit_book.find(over.unit_name) : nullptr;
        const bool red = over.unit_hostile && (picked == nullptr || g.outline_hostile(*picked));
        if (over.unit_entity != nullptr) {
            highlight_entity(over.unit_entity, true, red, g.rim_light_texture);
        }
        g.pad_highlighted = over.unit_entity;
        g.pad_highlighted_hostile = red;
    }
    if (over.unit_name != g.pad_selected) {
        if (over.unit_name.empty()) {
            TL_LOG_INFO("mouse over: nobody");
        } else {
            TL_LOG_INFO("mouse over: '%s'%s", over.unit_name.c_str(),
                        over.unit_hostile ? " (hostile)" : "");
        }
    }
    g.pad_selected = over.unit_name;
    g.pad_selected_hostile = over.unit_hostile;
    g.pad_selected_node = over.unit_node;
}

void Renderer::Impl::MouseGame::show_item(Ogre::Entity* entity) {
    if (entity == g.item_highlighted) return;
    if (g.item_highlighted != nullptr) {
        bool still_there = g.entity_still_placed(g.item_highlighted);
        for (const DroppedWorldItem& dropped : g.dropped_items) {
            if (dropped.entity == g.item_highlighted) still_there = true;
        }
        for (const BreakableWorld& b : g.breakables) {
            if (b.entity == g.item_highlighted) still_there = true;
        }
        if (still_there) highlight_entity(g.item_highlighted, false, false, g.rim_light_texture);
    }
    if (entity != nullptr) highlight_entity(entity, true, false, g.rim_light_texture);
    g.item_highlighted = entity;
}

/// What the floor labels and the port's item AI read of the selection.
void Renderer::Impl::MouseGame::note_item(const Over& over) {
    g.item_targeted = -1;
    g.trigger_targeted_machine = kNoMachine;
    g.trigger_targeted_node = 0;
    g.portal_targeted = -1;
    g.mimic_targeted_machine = kNoMachine;
    g.mimic_targeted_node = 0;
    g.item_second_pick = false;
    if (!over.have_item) return;
    const PlayerItemTarget& target = over.item.target;
    if (target.kind == Kind::Trigger) {
        g.trigger_targeted_machine = target.machine;
        g.trigger_targeted_node = target.node;
    } else if (target.kind == Kind::Mimic) {
        g.mimic_targeted_machine = target.machine;
        g.mimic_targeted_node = target.node;
    }
    for (size_t i = 0; i < g.dropped_items.size(); ++i) {
        if (target.kind == Kind::Dropped && g.dropped_items[i].node == target.dropped) {
            g.item_targeted = static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < g.open_portals.size(); ++i) {
        if (target.kind == Kind::Portal && g.open_portals[i].unit == target.portal) {
            g.portal_targeted = static_cast<int>(i);
        }
    }
}

// CGameClient::mouseOverCharacter: the character under the pointer, or
// with none the item.
void Renderer::Impl::MouseGame::update_over(bool active) {
    Over over;
    units.clear();
    if (active && g.camera != nullptr && input.pointer && g.subject_node != nullptr) {
        g.load_unit_types();
        const tl::core::ScreenMatrix matrix = screen_matrix();
        const float x = input.pointer_x * 2.0f - 1.0f;
        const float y = 1.0f - input.pointer_y * 2.0f;
        pick_character(matrix, x, y, over);
        if (over.unit_entity == nullptr) pick_item(matrix, x, y, over);
    }
    show_character(over);
    show_item(over.item_entity);
    const Unit over_item = over.have_item ? item_handle(over.item) : 0;
    if (over_item != client.over_item && over_item != 0) {
        const Kind kind = over.item.target.kind;
        TL_LOG_INFO("mouse over: %s", over.item.breakable != 0 ? "a breakable"
                                      : kind == Kind::Dropped  ? "a floor item"
                                      : kind == Kind::Portal   ? "a portal"
                                      : kind == Kind::Mimic    ? "a mimic"
                                                               : "a prop");
    }
    note_item(over);
    client.over_character = static_cast<Unit>(over.unit_node);
    client.over_item = over_item;
}

}  // namespace tl::render
