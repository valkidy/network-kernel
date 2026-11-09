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
// A gate nobody has walked through closes after this long.
constexpr float kGateUnusedTimeoutSeconds = 300.0f;
// SCENE_STATE is idempotent and small: re-sent this often so a client that
// missed one for any reason is never stuck on an old state.
constexpr float kSceneStateRefreshSeconds = 5.0f;

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

std::uint32_t GameServer::level_owner_net_id() const {
    const std::uint32_t host = host_player_net_id();
    if (host != 0u) return host;
    // Peer ids only go up, so the lowest is whoever has been here longest;
    // when that player leaves, the next one is the owner with nothing to track.
    return peer_players_.empty() ? 0u : peer_players_.begin()->second;
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
    level_template_id_ = found->id;
    level_ready_.clear();
    ready_wait_seconds_ = 0.0f;
    level_entered_ = false;
    abort_level_requested_ = false;
    gate_open_seconds_ = 0.0f;
    level_defeated_ = 0u;
    level_revives_granted_ = 0;
    level_announced_ = false;
    level_failed_.clear();
    recipe_owed_peers_.clear();
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
        const std::uint32_t owner = level_owner_net_id();
        const std::uint16_t id = get_u16(message.payload);
        const auto found = std::find_if(
            config_.scene_templates.begin(), config_.scene_templates.end(),
            [id](const SceneTemplateRef& ref) { return ref.id == id; });
        if (owner == 0u || message.player_net_id != owner || found == config_.scene_templates.end()) {
            spdlog::warn("scene select refused peer={} id={}", message.peer, id);
            return;
        }
        request_level(found->name);
    } else if (message.message_type == GAME_SERVER_MESSAGE_SCENE_RECIPE_REQUEST && message.payload_size == 5u) {
        if (message.payload[0] == kPartyLevelSlot &&
            get_u32(message.payload + 1) == scene_generation(kPartyLevelSlot) &&
            scene_state(kPartyLevelSlot) == SceneSlotState::kReady) {
            recipe_owed_peers_.insert(message.peer);
        }
    } else if (message.message_type == GAME_SERVER_MESSAGE_SCENE_LOAD_FAILED && message.payload_size == 6u) {
        if (message.payload[0] == kPartyLevelSlot && flow_ != GAME_SERVER_SCENE_FLOW_CAMP &&
            get_u32(message.payload + 1) == scene_generation(kPartyLevelSlot) && message.player_net_id != 0u) {
            spdlog::warn("player {} could not load the level, reason {}", message.player_net_id, message.payload[5]);
            level_failed_.insert(message.player_net_id);
        }
    } else if (message.message_type == GAME_SERVER_MESSAGE_SCENE_ABORT && message.payload_size == 0u) {
        const std::uint32_t owner = level_owner_net_id();
        if (owner == 0u || message.player_net_id != owner || flow_ == GAME_SERVER_SCENE_FLOW_CAMP) {
            spdlog::warn("scene abort refused peer={}", message.peer);
            return;
        }
        abort_level_requested_ = true;
    }
}

std::vector<std::uint8_t> GameServer::encode_scene_state() const {
    std::vector<std::uint8_t> out;
    // Listed once the server has it: until then a generated map has no recipe.
    const bool has_level = flow_ != GAME_SERVER_SCENE_FLOW_CAMP &&
        scene_state(kPartyLevelSlot) == SceneSlotState::kReady;
    put_u8(&out, flow_);
    put_u8(&out, flow_ != GAME_SERVER_SCENE_FLOW_CAMP ? static_cast<std::uint8_t>(kPartyLevelSlot) : 0u);
    put_u32(&out, scene_generation(kPartyLevelSlot));
    put_u8(&out, flow_outcome_);
    put_u8(&out, gate_.open ? 1u : 0u);
    put_f32(&out, gate_.position.x);
    put_f32(&out, gate_.position.y);
    put_f32(&out, gate_.position.z);
    put_f32(&out, gate_.radius);
    put_u32(&out, level_owner_net_id());
    const GameServerGameplayConfig* level_config = scene_config(kPartyLevelSlot);
    put_u16(&out, static_cast<std::uint16_t>(std::min<std::uint32_t>(level_defeated_, 0xffffu)));
    put_u16(&out, static_cast<std::uint16_t>(level_config != nullptr ? level_config->objective.defeat_enemies : 0u));
    put_u8(&out, has_level ? 1u : 0u);
    if (has_level) {
        const auto found = std::find_if(
            config_.scene_templates.begin(), config_.scene_templates.end(),
            [this](const SceneTemplateRef& ref) { return ref.id == level_template_id_; });
        const std::string& name = found->name;
        put_u8(&out, static_cast<std::uint8_t>(kPartyLevelSlot));
        put_u32(&out, scene_generation(kPartyLevelSlot));
        put_u16(&out, level_template_id_);
        const SceneSlot& level_slot = scene_slots_[kPartyLevelSlot];
        put_u8(&out, level_slot.recipe.empty() ? GAME_SERVER_SCENE_SOURCE_BUNDLE : GAME_SERVER_SCENE_SOURCE_RECIPE);
        put_u32(&out, static_cast<std::uint32_t>(level_slot.recipe_hash));
        put_u32(&out, static_cast<std::uint32_t>(level_slot.recipe_hash >> 32));
        put_u8(&out, static_cast<std::uint8_t>(name.size()));
        out.insert(out.end(), name.begin(), name.end());
    }
    return out;
}

void GameServer::flush_scene_state() {
    if (kernel_ == nullptr || scene_state_owed_.empty()) return;
    const std::vector<std::uint8_t> body = encode_scene_state();
    for (auto it = scene_state_owed_.begin(); it != scene_state_owed_.end();) {
        const bool gone = peer_players_.count(*it) == 0u;
        if (gone || Kernel_ServerSendGameMessage(kernel_, *it, GAME_SERVER_MESSAGE_SCENE_STATE,
                body.data(), static_cast<std::uint32_t>(body.size()))) {
            it = scene_state_owed_.erase(it);
        } else {
            ++it;
        }
    }
}

void GameServer::flush_scene_recipes() {
    if (recipe_owed_peers_.empty()) return;
    const SceneSlot& slot = scene_slots_[kPartyLevelSlot];
    if (slot.state != SceneSlotState::kReady || slot.recipe.empty()) {
        recipe_owed_peers_.clear();
        return;
    }
    for (auto it = recipe_owed_peers_.begin(); it != recipe_owed_peers_.end();) {
        bool sent = peer_players_.count(*it) != 0u;
        if (sent) {
            // A transfer is all or nothing: one that fails half way starts
            // over next tick under a new id, which replaces the partial one.
            const auto chunks = encode_scene_recipe_chunks(
                kPartyLevelSlot, slot.generation, next_recipe_transfer_id_++, slot.recipe);
            for (const std::vector<std::uint8_t>& chunk : chunks) {
                if (!Kernel_ServerSendGameMessage(kernel_, *it, GAME_SERVER_MESSAGE_SCENE_RECIPE_CHUNK,
                        chunk.data(), static_cast<std::uint32_t>(chunk.size()))) {
                    sent = false;
                    break;
                }
            }
            if (!sent) {
                ++it;
                continue;
            }
        }
        it = recipe_owed_peers_.erase(it);
    }
}

void GameServer::mark_scene_state_owed() {
    for (const auto& [peer, player] : peer_players_) {
        (void)player;
        scene_state_owed_.insert(peer);
    }
}

void GameServer::broadcast_scene_state() {
    mark_scene_state_owed();
    flush_scene_state();
}

void GameServer::note_level_enemy_death(std::uint32_t net_id) {
    if (flow_ != GAME_SERVER_SCENE_FLOW_GATE_OPEN) return;
    const AgentRuntimeManager* level = scene_manager(kPartyLevelSlot);
    if (level == nullptr || !level->is_enemy_agent(net_id)) return;
    ++level_defeated_;
    mark_scene_state_owed();  // sent next tick, however many fell this one
}

void GameServer::grant_late_joiner_revives() {
    const GameServerGameplayConfig* level = scene_config(kPartyLevelSlot);
    if (!level_entered_ || level == nullptr || level->player.respawn.revives_per_player <= 0) {
        return;
    }
    const std::int32_t share = std::min(
        level->player.respawn.revives_per_player,
        level->player.respawn.max_team_revives - level_revives_granted_);
    if (share > 0) {
        respawn_.grant(share);
        level_revives_granted_ += share;
    }
}

void GameServer::place_level_nests() {
    const GameServerGameplayConfig* level = scene_config(kPartyLevelSlot);
    const AgentRuntimeManager* level_ai = scene_manager(kPartyLevelSlot);
    if (level == nullptr || level_ai == nullptr) return;
    std::uint32_t placed = 0;
    for (const KernelVec3& position : level->level_nests.positions) {
        // Only where the party can walk to it: a nest on a walled-in stamp
        // would be an enemy nobody can reach.
        PatrolRouteRequest route;
        route.start = spawn_of(*level);
        route.end = position;
        route.snap_half_extent_meters = 3.0f;
        std::vector<KernelVec3> waypoints;
        if (!level_ai->patrol_navigation().find_route(route, &waypoints)) {
            spdlog::warn("level nest at ({}, {}, {}) is unreachable; skipped", position.x, position.y, position.z);
            continue;
        }
        KernelServerEntityCreateInfo create{};
        create.struct_size = sizeof(create);
        create.entity_template_id = level->level_nests.entity_template_id;
        create.position = position;
        create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t net_id = 0;
        if (Kernel_ServerCreateEntity(kernel_, &create, &net_id)) ++placed;
    }
    spdlog::info("level nests placed {} of {}", placed, level->level_nests.positions.size());
}

bool GameServer::level_objective_met() const {
    const GameServerGameplayConfig* level = scene_config(kPartyLevelSlot);
    const AgentRuntimeManager* level_ai = scene_manager(kPartyLevelSlot);
    if (level == nullptr || level_ai == nullptr) return false;
    if (level->objective.defeat_enemies != 0u) {
        return level_defeated_ >= level->objective.defeat_enemies;
    }
    const auto& rules = level_ai->game_rule_director().runtimes();
    return !rules.empty() &&
        std::all_of(rules.begin(), rules.end(), [](const GameRuleDirector::RuleRuntime& rule) {
            return rule.status == GameRuleStatus::kCompleted;
        });
}

void GameServer::return_level_players_to_camp(const std::vector<KernelServerEntityState>& players) {
    const SceneBounds level_bounds = scene_slot_bounds(kPartyLevelSlot);
    std::uint32_t back = 0;
    for (const KernelServerEntityState& player : players) {
        if (!level_bounds.contains(player.position.x)) continue;
        teleport(kernel_, player.net_id,
            plus(spawn_of(config_), KernelVec3{0.0f, 0.0f, kArrivalSpacing * static_cast<float>(back++)}));
        if (player.hp != 0u) continue;
        KernelServerReviveInfo info{};
        info.struct_size = sizeof(info);
        info.net_id = player.net_id;
        info.lift_meters = 0.0f;
        if (Kernel_ServerReviveEntity(kernel_, &info)) {
            configure_player(player.net_id, true);
        }
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
    abort_level_requested_ = false;
    recipe_owed_peers_.clear();
    gate_.open = false;
    flow_outcome_ = outcome;
    spdlog::info("party level finished outcome={}", outcome);
    set_flow(GAME_SERVER_SCENE_FLOW_CAMP);
}

void GameServer::tick_scene_flow(float delta_seconds) {
    if (!scene_table_enabled_ || kernel_ == nullptr) {
        return;
    }
    scene_state_refresh_seconds_ += delta_seconds;
    if (scene_state_refresh_seconds_ >= kSceneStateRefreshSeconds) {
        scene_state_refresh_seconds_ = 0.0f;
        for (const auto& [peer, player] : peer_players_) {
            (void)player;
            scene_state_owed_.insert(peer);
        }
    }
    flush_scene_state();
    flush_scene_recipes();
    if (flow_ == GAME_SERVER_SCENE_FLOW_CAMP) {
        if (!auto_level_.empty() && !peer_players_.empty() &&
            flow_outcome_ != GAME_SERVER_SCENE_OUTCOME_LOAD_FAILED) {
            request_level(auto_level_);
        }
        return;
    }
    // Nobody left to play it (only a dedicated server can get here).
    if (peer_players_.empty()) {
        abort_level_requested_ = true;
    }
    const SceneSlotState slot_state = scene_state(kPartyLevelSlot);
    if (flow_ == GAME_SERVER_SCENE_FLOW_LOADING) {
        if (slot_state == SceneSlotState::kEmpty) {
            finish_level(GAME_SERVER_SCENE_OUTCOME_LOAD_FAILED);
            return;
        }
        if (slot_state != SceneSlotState::kReady) return;
        if (abort_level_requested_) {
            finish_level(GAME_SERVER_SCENE_OUTCOME_ABORTED);
            return;
        }
        if (!level_announced_) {
            level_announced_ = true;
            broadcast_scene_state();
        }
        ready_wait_seconds_ += delta_seconds;
        bool everyone_ready = !peer_players_.empty();
        bool everyone_failed = !peer_players_.empty();
        for (const auto& [peer, player] : peer_players_) {
            (void)peer;
            const bool failed = level_failed_.count(player) != 0u;
            if (level_ready_.count(player) == 0u && !failed) everyone_ready = false;
            if (!failed) everyone_failed = false;
        }
        if (everyone_failed) {
            finish_level(GAME_SERVER_SCENE_OUTCOME_LOAD_FAILED);
            return;
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
    if (abort_level_requested_) {
        return_level_players_to_camp(players);
        finish_level(GAME_SERVER_SCENE_OUTCOME_ABORTED);
        return;
    }

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
            PlayerRespawnConfig pool = level->player.respawn;
            if (pool.revives_per_player > 0) {
                level_revives_granted_ = std::min(
                    static_cast<std::int32_t>(peer_players_.size()) * pool.revives_per_player,
                    pool.max_team_revives);
                pool.team_revive_times = level_revives_granted_;
            }
            respawn_.reset(pool);
            level_entered_ = true;
            place_level_nests();
        }
        gate_open_seconds_ += delta_seconds;
        if (!level_entered_ && gate_open_seconds_ >= kGateUnusedTimeoutSeconds) {
            spdlog::info("party level gate unused for {} s; level cancelled", kGateUnusedTimeoutSeconds);
            finish_level(GAME_SERVER_SCENE_OUTCOME_ABORTED);
            return;
        }
        if (level_objective_met()) {
            gate_.position = level->has_return_gate_position
                ? level->return_gate_position
                : plus(spawn_of(*level), kGateOffset);
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
            return_level_players_to_camp(players);
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
