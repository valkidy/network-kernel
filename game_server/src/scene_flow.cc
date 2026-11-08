// The party's way through levels (docs/SCENE_TABLE_PLAN.md S4.3-S4.6); see
// GameServer::request_level. Kept apart from game_server.cc, which it only
// extends.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

#include "game_server/src/game_server.h"

namespace network_example::game_server {
namespace {

// Players a level may wait for before its gate opens anyway (Q4). One who has
// not reported ready by then still cannot go through; nobody is kicked.
constexpr float kReadyTimeoutSeconds = 60.0f;
// Where a gate stands: this far along +x from the spawn it leads away from.
constexpr KernelVec3 kGateOffset{3.0f, 0.0f, 0.0f};
// Arrivals are spread along z so a party does not land in one spot.
constexpr float kArrivalSpacing = 1.5f;
constexpr float kGateHeightTolerance = 3.0f;
constexpr std::size_t kMaxTemplateName = 32u;

void put_u8(std::vector<std::uint8_t>* out, std::uint8_t v) { out->push_back(v); }
void put_u16(std::vector<std::uint8_t>* out, std::uint16_t v) {
    out->push_back(static_cast<std::uint8_t>(v));
    out->push_back(static_cast<std::uint8_t>(v >> 8));
}
void put_u32(std::vector<std::uint8_t>* out, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) out->push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}
void put_f32(std::vector<std::uint8_t>* out, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(out, bits);
}
std::uint16_t get_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t get_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

KernelVec3 plus(const KernelVec3& a, const KernelVec3& b) {
    return KernelVec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

KernelVec3 spawn_of(const GameServerGameplayConfig& config) {
    return config.player.has_spawn_position ? config.player.spawn_position : config.scene_offset;
}

bool at_gate(const GameServer::SceneGate& gate, const KernelVec3& p) {
    const float dx = p.x - gate.position.x;
    const float dz = p.z - gate.position.z;
    return gate.open && dx * dx + dz * dz <= gate.radius * gate.radius &&
        std::fabs(p.y - gate.position.y) <= kGateHeightTolerance;
}

std::vector<KernelServerEntityState> player_states(KernelHandle* kernel) {
    std::vector<KernelServerEntityState> states(64);
    while (true) {
        for (KernelServerEntityState& state : states) state.struct_size = sizeof(state);
        const std::uint32_t count = Kernel_ServerQueryEntities(
            kernel, KernelEntityType_Actor, states.data(), static_cast<std::uint32_t>(states.size()));
        if (count < states.size() || states.size() >= 65536u) {
            states.resize(count);
            break;
        }
        states.resize(states.size() * 2);
    }
    states.erase(
        std::remove_if(states.begin(), states.end(), [](const KernelServerEntityState& s) {
            return s.actor_type != KernelActorType_Player;
        }),
        states.end());
    return states;
}

void teleport(KernelHandle* kernel, std::uint32_t net_id, const KernelVec3& to) {
    const KernelQuat facing{0.0f, 0.0f, 0.0f, 1.0f};
    Kernel_ServerSetEntityTransform(kernel, net_id, &to, &facing);
}

}  // namespace

std::uint32_t GameServer::host_player_net_id() const {
    KernelLocalPlayerInfo local{};
    if (kernel_ == nullptr || !Kernel_GetLocalPlayerInfo(kernel_, &local) || local.has_welcome == 0u) {
        return 0u;  // a dedicated server has no player of its own
    }
    return local.player_net_id;
}

AgentRuntimeManager& GameServer::manager_for_agent(std::uint32_t net_id) {
    for (SceneSlot& slot : scene_slots_) {
        if (slot.manager == nullptr) continue;
        for (const AgentRuntimeState& agent : slot.manager->agents()) {
            if (agent.net_id == net_id) return *slot.manager;
        }
    }
    return agent_runtime_manager_;
}

bool GameServer::request_level(const std::string& scene_template) {
    if (!scene_table_enabled_ || flow_ != GAME_SERVER_SCENE_FLOW_CAMP) {
        return false;
    }
    const auto found = std::find_if(
        config_.scene_templates.begin(), config_.scene_templates.end(),
        [&](const SceneTemplateRef& ref) { return ref.name == scene_template; });
    if (found == config_.scene_templates.end() || scene_template.size() > kMaxTemplateName ||
        !load_scene(kPartyLevelSlot, scene_template)) {
        return false;
    }
    level_template_index_ = static_cast<std::uint16_t>(found - config_.scene_templates.begin());
    level_ready_.clear();
    ready_wait_seconds_ = 0.0f;
    level_entered_ = false;
    flow_outcome_ = GAME_SERVER_SCENE_OUTCOME_NONE;
    gate_.open = false;
    set_flow(GAME_SERVER_SCENE_FLOW_LOADING);
    spdlog::info("party level requested template={} generation={}",
        scene_template, scene_generation(kPartyLevelSlot));
    return true;
}

void GameServer::handle_scene_message(const KernelGameMessage& message) {
    if (!scene_table_enabled_) return;
    if (message.message_type == GAME_SERVER_MESSAGE_SCENE_READY && message.payload_size == 5u) {
        const std::uint32_t slot = message.payload[0];
        const std::uint32_t generation = get_u32(message.payload + 1);
        if (slot == kPartyLevelSlot && flow_ != GAME_SERVER_SCENE_FLOW_CAMP &&
            generation == scene_generation(kPartyLevelSlot) && message.player_net_id != 0u) {
            level_ready_.insert(message.player_net_id);
        }
    } else if (message.message_type == GAME_SERVER_MESSAGE_SCENE_SELECT && message.payload_size == 2u) {
        const std::uint32_t host = host_player_net_id();
        const std::uint16_t index = get_u16(message.payload);
        if (host == 0u || message.player_net_id != host || index >= config_.scene_templates.size()) {
            spdlog::warn("scene select refused peer={} index={}", message.peer, index);
            return;
        }
        request_level(config_.scene_templates[index].name);
    }
}

std::vector<std::uint8_t> GameServer::encode_scene_state() const {
    std::vector<std::uint8_t> out;
    const bool has_level = flow_ != GAME_SERVER_SCENE_FLOW_CAMP;
    put_u8(&out, flow_);
    put_u8(&out, has_level ? static_cast<std::uint8_t>(kPartyLevelSlot) : 0u);
    put_u32(&out, scene_generation(kPartyLevelSlot));
    put_u8(&out, flow_outcome_);
    put_u8(&out, gate_.open ? 1u : 0u);
    put_f32(&out, gate_.position.x);
    put_f32(&out, gate_.position.y);
    put_f32(&out, gate_.position.z);
    put_f32(&out, gate_.radius);
    put_u8(&out, has_level ? 1u : 0u);
    if (has_level) {
        const std::string& name = config_.scene_templates[level_template_index_].name;
        put_u8(&out, static_cast<std::uint8_t>(kPartyLevelSlot));
        put_u32(&out, scene_generation(kPartyLevelSlot));
        put_u16(&out, level_template_index_);
        put_u8(&out, static_cast<std::uint8_t>(name.size()));
        out.insert(out.end(), name.begin(), name.end());
    }
    return out;
}

void GameServer::send_scene_state(std::uint32_t peer) const {
    if (kernel_ == nullptr) return;
    const std::vector<std::uint8_t> body = encode_scene_state();
    Kernel_ServerSendGameMessage(kernel_, peer, GAME_SERVER_MESSAGE_SCENE_STATE,
        body.data(), static_cast<std::uint32_t>(body.size()));
}

void GameServer::broadcast_scene_state() const {
    for (const auto& [peer, player] : peer_players_) {
        (void)player;
        send_scene_state(peer);
    }
}

void GameServer::set_flow(std::uint8_t flow) {
    flow_ = flow;
    broadcast_scene_state();
}

void GameServer::finish_level(std::uint8_t outcome) {
    respawn_.reset(config_.player.respawn);
    if (scene_state(kPartyLevelSlot) == SceneSlotState::kReady) {
        unload_scene(kPartyLevelSlot);
    }
    level_ready_.clear();
    level_entered_ = false;
    gate_.open = false;
    flow_outcome_ = outcome;
    spdlog::info("party level finished outcome={}", outcome);
    set_flow(GAME_SERVER_SCENE_FLOW_CAMP);
}

void GameServer::tick_scene_flow(float delta_seconds) {
    if (!scene_table_enabled_ || kernel_ == nullptr || flow_ == GAME_SERVER_SCENE_FLOW_CAMP) {
        return;
    }
    const SceneSlotState slot_state = scene_state(kPartyLevelSlot);
    if (flow_ == GAME_SERVER_SCENE_FLOW_LOADING) {
        if (slot_state == SceneSlotState::kEmpty) {
            finish_level(GAME_SERVER_SCENE_OUTCOME_LOAD_FAILED);
            return;
        }
        if (slot_state != SceneSlotState::kReady) return;
        ready_wait_seconds_ += delta_seconds;
        bool everyone_ready = !peer_players_.empty();
        for (const auto& [peer, player] : peer_players_) {
            (void)peer;
            if (level_ready_.count(player) == 0u) everyone_ready = false;
        }
        if (everyone_ready || ready_wait_seconds_ >= kReadyTimeoutSeconds) {
            if (!everyone_ready) {
                spdlog::warn("party level gate opens without every player ready ({} of {})",
                    level_ready_.size(), peer_players_.size());
            }
            gate_.open = true;
            gate_.position = plus(spawn_of(config_), kGateOffset);
            set_flow(GAME_SERVER_SCENE_FLOW_GATE_OPEN);
        }
        return;
    }

    const GameServerGameplayConfig* level = scene_config(kPartyLevelSlot);
    const AgentRuntimeManager* level_ai = scene_manager(kPartyLevelSlot);
    if (level == nullptr || level_ai == nullptr) {
        finish_level(GAME_SERVER_SCENE_OUTCOME_LOAD_FAILED);
        return;
    }
    const SceneBounds level_bounds = scene_slot_bounds(kPartyLevelSlot);
    const std::vector<KernelServerEntityState> players = player_states(kernel_);
    std::uint32_t arrivals = 0;

    if (flow_ == GAME_SERVER_SCENE_FLOW_GATE_OPEN) {
        for (const KernelServerEntityState& player : players) {
            if (player.hp == 0u || level_ready_.count(player.net_id) == 0u ||
                !at_gate(gate_, player.position)) {
                continue;
            }
            teleport(kernel_, player.net_id,
                plus(spawn_of(*level), KernelVec3{0.0f, 0.0f, kArrivalSpacing * static_cast<float>(arrivals++)}));
        }
        // The party's revives are the level's from the moment anyone is in
        // it -- through the gate or any other way.
        if (!level_entered_ && (arrivals != 0u ||
                std::any_of(players.begin(), players.end(), [&](const KernelServerEntityState& p) {
                    return level_bounds.contains(p.position.x);
                }))) {
            respawn_.reset(level->player.respawn);
            level_entered_ = true;
        }
        const auto& rules = level_ai->game_rule_director().runtimes();
        const bool completed = !rules.empty() &&
            std::all_of(rules.begin(), rules.end(), [](const GameRuleDirector::RuleRuntime& rule) {
                return rule.status == GameRuleStatus::kCompleted;
            });
        if (completed) {
            gate_.position = plus(spawn_of(*level), kGateOffset);
            flow_outcome_ = GAME_SERVER_SCENE_OUTCOME_COMPLETED;
            spdlog::info("party level completed; return gate open");
            set_flow(GAME_SERVER_SCENE_FLOW_RETURN_OPEN);
            return;
        }
        // Out of revives with everyone in the level dead and nobody's revive
        // still due: the level is lost.
        std::uint32_t inside = 0;
        bool anyone_alive = false;
        for (const KernelServerEntityState& player : players) {
            if (!level_bounds.contains(player.position.x)) continue;
            ++inside;
            if (player.hp != 0u || respawn_.is_waiting(player.net_id)) anyone_alive = true;
        }
        if (level_entered_ && inside != 0u && !anyone_alive && respawn_.remaining_revives() == 0) {
            std::uint32_t back = 0;
            for (const KernelServerEntityState& player : players) {
                if (!level_bounds.contains(player.position.x)) continue;
                teleport(kernel_, player.net_id,
                    plus(spawn_of(config_), KernelVec3{0.0f, 0.0f, kArrivalSpacing * static_cast<float>(back++)}));
                KernelServerReviveInfo info{};
                info.struct_size = sizeof(info);
                info.net_id = player.net_id;
                info.lift_meters = 0.0f;
                if (Kernel_ServerReviveEntity(kernel_, &info)) {
                    configure_player(player.net_id, true);
                }
            }
            finish_level(GAME_SERVER_SCENE_OUTCOME_FAILED);
        }
        return;
    }

    if (flow_ == GAME_SERVER_SCENE_FLOW_RETURN_OPEN) {
        std::uint32_t inside = 0;
        for (const KernelServerEntityState& player : players) {
            if (!level_bounds.contains(player.position.x)) continue;
            if (player.hp != 0u && at_gate(gate_, player.position)) {
                teleport(kernel_, player.net_id,
                    plus(spawn_of(config_), KernelVec3{0.0f, 0.0f, kArrivalSpacing * static_cast<float>(arrivals++)}));
                continue;
            }
            ++inside;
        }
        if (inside == 0u) {
            finish_level(GAME_SERVER_SCENE_OUTCOME_COMPLETED);
        }
    }
}

}  // namespace network_example::game_server
