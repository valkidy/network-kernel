#include <array>
#include <cassert>
#include <cstdint>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/public/kernel_api.h"

#define private public
#include "kernel/src/kernel.h"
#include "simulation/src/systems.h"
#undef private

namespace {

KernelServerEntityCreateInfo player_create_info() {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = static_cast<std::uint16_t>(network_example::EntityType::kActor);
    create_info.actor_type = KernelActorType_Player;
    create_info.owner_peer = 42;
    create_info.position = KernelVec3{2.0f, 0.0f, 3.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    create_info.animation_state = 7;
    create_info.visual_flags = 0x44u;
    return create_info;
}

void lifecycle_system_create_matches_legacy_path() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    network_example::EntityLifecycleSystem lifecycle;
    std::uint32_t net_id = 0;
    assert(lifecycle.create_entity(engine, player_create_info(), &net_id));
    assert(net_id != 0);

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    assert(engine.server_get_entity_state(net_id, &state));
    assert(state.valid != 0u);
    assert(state.net_id == net_id);
    assert(state.entity_type == static_cast<std::uint16_t>(network_example::EntityType::kActor));
    assert(state.actor_type == KernelActorType_Player);
    assert(state.owner_peer == 42);
    assert(state.animation_state == 7);
    assert(state.visual_flags == 0x44u);
    assert(state.position.x == 2.0f);
    assert(state.position.z == 3.0f);
    assert(engine.events_.size() == 1);
    assert(engine.events_[0].type == KernelEventType_EntitySpawned);
    assert(engine.latest_snapshot_.entities.size() == 1);
}

void lifecycle_system_destroy_matches_legacy_side_effects() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    network_example::KernelEngine engine(config);
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    std::uint32_t net_id = 0;
    assert(engine.server_create_entity(player_create_info(), &net_id));
    engine.events_.clear();
    engine.lifecycle_events_.clear();
    engine.vision_configs_[net_id] = KernelAgentVisionConfig{};
    engine.vision_states_[net_id] = network_example::KernelEngine::VisionRuntimeState{};

    network_example::EntityLifecycleSystem lifecycle;
    assert(lifecycle.destroy_entity(engine, net_id, KernelDespawnReason_Destroyed));

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    assert(!engine.server_get_entity_state(net_id, &state));
    assert(engine.vision_configs_.find(net_id) == engine.vision_configs_.end());
    assert(engine.vision_states_.find(net_id) == engine.vision_states_.end());
    assert(engine.events_.size() == 1);
    assert(engine.events_[0].type == KernelEventType_EntityDestroyed);
    assert(engine.lifecycle_events_.size() == 1);
    assert(engine.lifecycle_events_[0].net_id == net_id);
    assert(engine.lifecycle_events_[0].reason == KernelDespawnReason_Destroyed);
    assert(engine.latest_snapshot_.entities.empty());
}

}  // namespace

int main() {
    lifecycle_system_create_matches_legacy_path();
    lifecycle_system_destroy_matches_legacy_side_effects();
    return 0;
}
