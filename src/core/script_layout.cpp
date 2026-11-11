// The level machine's layout starts and what rides on them: a linked layout's timelines played (the
// load end's and the activation's restart), a particle link's doubled outputs, a Monster's quest
// marker. Split from script.cpp.
#include "core/script.h"

#include "core/logic_events.h"

namespace tl::core {
namespace {

namespace type = tl::formats::cmp_type;
using namespace tl::core::logic;

}  // namespace

void LevelScript::play_layout_timelines(QuestState& quests, std::vector<ScriptEvent>& out,
                                        bool backwards, bool restart) {
    std::vector<int64_t> driven;
    for (const Object& o : objects_) {
        if (o.timeline != nullptr && o.tl_layout_control) driven.push_back(o.id);
    }
    for (int64_t id : driven) {
        Object* o = find(id);
        if (o == nullptr) continue;
        if (restart && !backwards) {
            // state 0: `Reset_To_Beginning` (nothing while the time
            // is still -0.01), `+0x81` (playing) cleared, then `CTimeline::Play(t, 1)` - whose
            // percent of -0.01 is clamped to 0 by the track update, so a key at 0
            // fires now (a Random Choice's `Roll` in `palace_props/JacobsLadder2`).
            if (o->tl_time != -0.01f) {
                timeline_reset_properties(*o, -0.01f);
                timeline_set_percent(*o, -0.01f, quests, out);
                broadcast(o->id, kResetToBeginning, quests, out);
            }
            o->tl_playing = false;
        }
        if (backwards) timeline_play_backwards(*o, quests, out);
        else timeline_play(*o, quests, out);
    }
}

// `CLayoutDescriptor::InputLogicEvent`'s Start / Stop Particle:
// a particle link's start or stop (states 0 and 1 with its `CParticle`
// +0x188) runs `CParticle::Start` / `Stop` and raises the output itself first,
// then the input's case raises it again.
void LevelScript::link_output(const Object& o, uint32_t output, QuestState& quests,
                              std::vector<ScriptEvent>& out) {
    if (o.type == type::kParticleLink && output == kParticleStarted) {
        // `CParticle::Start`'s sounds, before the output.
        ScriptEvent e;
        e.action = ScriptAction::ParticleLinkStarted;
        e.node_id = o.id;
        e.name = o.name;
        e.position = o.position;
        out.push_back(e);
    }
    if (o.type == type::kParticleLink) broadcast(o.id, output, quests, out);
    broadcast(o.id, output, quests, out);
}

// `CMonsterDescriptor::InputLogicEvent`'s Show, Hide, Enable and Disable end in
// `CQuestManager::calculateNPCIcon` for the character, its quest drawn.
void LevelScript::monster_npc_icon(const Object& o) {
    if (npc_icon_ && character(o.id) != nullptr) npc_icon_(o.unit);
}

// PORT: an accessor of the character record.
bool LevelScript::unit_mesh_hidden(int64_t id) const {
    const Character* c = character(id);
    return c != nullptr && c->mesh_hidden;
}

}  // namespace tl::core
