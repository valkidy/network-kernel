#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include <entt/entt.hpp>

#include "kernel/public/kernel_api.h"
#include "kernel/src/kernel_api_internal.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

KernelConfig server_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    return config;
}

KernelServerEntityCreateInfo player_create_info() {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = static_cast<std::uint16_t>(network_example::EntityType::kActor);
    create_info.actor_type = KernelActorType_Player;
    create_info.owner_peer = 7;
    create_info.position = KernelVec3{1.0f, 0.0f, 2.0f};
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    return create_info;
}

void queued_transform_applies_on_next_tick() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartDedicatedServer(kernel, 7791));

    std::uint32_t net_id = 0;
    KernelServerEntityCreateInfo create_info = player_create_info();
    assert(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));

    KernelVec3 position{5.0f, 0.0f, 6.0f};
    KernelQuat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    assert(Kernel_ServerEnqueueEntityTransform(
        kernel,
        KernelCommandSource_Test,
        net_id,
        &position,
        &rotation));

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &state));
    assert(state.position.x == 1.0f);

    Kernel_Update(kernel, 1.0f / 30.0f);
    state = KernelServerEntityState{};
    state.struct_size = sizeof(state);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &state));
    assert(state.position.x == 5.0f);
    assert(state.position.z == 6.0f);

    Kernel_Destroy(kernel);
}

void queued_lifecycle_destroy_applies_on_next_tick() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartDedicatedServer(kernel, 7792));

    std::uint32_t net_id = 0;
    KernelServerEntityCreateInfo create_info = player_create_info();
    assert(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));

    KernelEntityLifecycleCommand command{};
    command.struct_size = sizeof(command);
    command.command_type = KernelEntityLifecycleCommandType_Destroy;
    command.net_id = net_id;
    command.reason = KernelDespawnReason_Destroyed;
    assert(Kernel_ServerEnqueueEntityLifecycle(
        kernel,
        KernelCommandSource_Test,
        &command));

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &state));

    Kernel_Update(kernel, 1.0f / 30.0f);
    state = KernelServerEntityState{};
    state.struct_size = sizeof(state);
    assert(!Kernel_ServerGetEntityState(kernel, net_id, &state));

    Kernel_Destroy(kernel);
}

void queued_submit_input_drains_before_movement() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartDedicatedServer(kernel, 7793));

    std::uint32_t net_id = 0;
    KernelServerEntityCreateInfo create_info = player_create_info();
    assert(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));

    PlayerInput input{};
    input.input_seq = 11;
    input.move = KernelVec2{1.0f, 0.0f};
    assert(Kernel_ServerEnqueueEntityInput(
        kernel,
        KernelCommandSource_Test,
        net_id,
        &input));

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &state));
    assert(state.position.x == 1.0f);

    Kernel_Update(kernel, 1.0f / 30.0f);
    state = KernelServerEntityState{};
    state.struct_size = sizeof(state);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &state));
    assert(state.position.x > 1.0f);

    Kernel_Destroy(kernel);
}

void queue_capacity_rejects_after_default_capacity() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartDedicatedServer(kernel, 7794));

    std::uint32_t net_id = 0;
    KernelServerEntityCreateInfo create_info = player_create_info();
    assert(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));

    for (std::size_t index = 0; index < 2048; ++index) {
        KernelVec3 velocity{0.0f, 0.0f, 0.0f};
        assert(Kernel_ServerEnqueueEntityVelocity(
            kernel,
            KernelCommandSource_Test,
            net_id,
            &velocity));
    }
    KernelVec3 velocity{1.0f, 0.0f, 0.0f};
    assert(!Kernel_ServerEnqueueEntityVelocity(
        kernel,
        KernelCommandSource_Test,
        net_id,
        &velocity));

    Kernel_Destroy(kernel);
}

void queue_warning_threshold_records_warning_once() {
    network_example::KernelEngine engine(server_config());
    engine.reset_runtime_state(KernelMode_DedicatedServer);

    KernelVec3 velocity{0.0f, 0.0f, 0.0f};
    for (std::size_t index = 0; index < 1600; ++index) {
        assert(engine.server_enqueue_entity_velocity(
            KernelCommandSource_Test,
            77,
            velocity));
    }

    assert(engine.command_queue_.size() == 1600);
    assert(engine.command_queue_capacity_warning_count_ == 1);
}

void tick_monitor_records_warning_when_threshold_is_low() {
    network_example::KernelEngine engine(server_config());
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    engine.simulation_tick_cost_warning_threshold_us_ = 1;

    engine.simulate_tick();

    assert(engine.last_simulation_tick_cost_us_ >= 1);
    assert(engine.simulation_tick_cost_warning_count_ == 1);
}

}  // namespace

int main() {
    queued_transform_applies_on_next_tick();
    queued_lifecycle_destroy_applies_on_next_tick();
    queued_submit_input_drains_before_movement();
    queue_capacity_rejects_after_default_capacity();
    queue_warning_threshold_records_warning_once();
    tick_monitor_records_warning_when_threshold_is_low();
    return 0;
}
