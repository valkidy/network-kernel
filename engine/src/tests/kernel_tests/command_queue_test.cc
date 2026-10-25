#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include <entt/entt.hpp>

#include "kernel/public/kernel_api.h"
#include "kernel/src/kernel_api_internal.h"

#define private public
#include "kernel/src/kernel.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

}  // namespace

#define require(condition) \
    require_impl(static_cast<bool>(condition), #condition, __LINE__)
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
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7791));

    std::uint32_t net_id = 0;
    KernelServerEntityCreateInfo create_info = player_create_info();
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));

    KernelVec3 position{5.0f, 0.0f, 6.0f};
    KernelQuat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerEnqueueEntityTransform(
        kernel,
        KernelCommandSource_Test,
        net_id,
        &position,
        &rotation));

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    require(state.position.x == 1.0f);

    Kernel_Update(kernel, 1.0f / 30.0f);
    state = KernelServerEntityState{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    require(state.position.x == 5.0f);
    require(state.position.z == 6.0f);

    Kernel_Destroy(kernel);
}

// The AI re-aims an agent every tick by queueing the position it read with a
// new rotation. That is not a teleport, so the character controller's
// vertical anchor survives it; a real move still drops it. Dropping it on the
// re-aim seated a legged rig's capsule at its body height, buried, and the
// garrison quadruped stalled a few metres into its route.
void queued_rotation_keeps_the_controller_anchor() {
    network_example::KernelEngine engine(server_config());
    require(engine.start_dedicated_server(7795));

    std::uint32_t net_id = 0;
    require(engine.server_create_entity(player_create_info(), &net_id));
    const std::optional<entt::entity> entity = engine.world_.find_entity(net_id);
    require(entity.has_value());
    // A template-less player is kNone: the movement pass leaves its anchor
    // alone, so only the transform command can change it here.
    auto& movement =
        engine.world_.registry().get<network_example::MovementState>(*entity);
    movement.controller_height = 0.5f;
    movement.has_controller_height = true;

    const KernelVec3 same_position{1.0f, 0.0f, 2.0f};
    const KernelQuat turned{0.0f, 0.7071068f, 0.0f, 0.7071068f};
    require(engine.server_enqueue_entity_transform(
        KernelCommandSource_AI, net_id, same_position, turned));
    engine.update(1.0f / 30.0f);
    const network_example::Transform& transform =
        engine.world_.registry().get<network_example::Transform>(*entity);
    require(transform.rotation.y > 0.7f);
    require(movement.has_controller_height);
    require(movement.controller_height == 0.5f);

    const KernelVec3 elsewhere{4.0f, 0.0f, 2.0f};
    require(engine.server_enqueue_entity_transform(
        KernelCommandSource_Test, net_id, elsewhere, turned));
    engine.update(1.0f / 30.0f);
    require(transform.position.x == 4.0f);
    require(!movement.has_controller_height);
}

void queued_lifecycle_destroy_applies_on_next_tick() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7792));

    std::uint32_t net_id = 0;
    KernelServerEntityCreateInfo create_info = player_create_info();
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));

    KernelEntityLifecycleCommand command{};
    command.struct_size = sizeof(command);
    command.command_type = KernelEntityLifecycleCommandType_Destroy;
    command.net_id = net_id;
    command.reason = KernelDespawnReason_Destroyed;
    require(Kernel_ServerEnqueueEntityLifecycle(
        kernel,
        KernelCommandSource_Test,
        &command));

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));

    Kernel_Update(kernel, 1.0f / 30.0f);
    state = KernelServerEntityState{};
    state.struct_size = sizeof(state);
    require(!Kernel_ServerGetEntityState(kernel, net_id, &state));

    Kernel_Destroy(kernel);
}

void queued_submit_player_input_drains_before_movement() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7793));

    std::uint32_t net_id = 0;
    KernelServerEntityCreateInfo create_info = player_create_info();
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));

    KernelPlayerInput input{};
    input.input_seq = 11;
    input.move = KernelVec2{1.0f, 0.0f};
    input.aim_dir = KernelVec3{0.0f, 0.0f, 1.0f};
    require(Kernel_ServerEnqueueEntityInput(
        kernel,
        KernelCommandSource_Test,
        net_id,
        &input));

    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    require(state.aim_direction.z != 1.0f);

    Kernel_Update(kernel, 1.0f / 30.0f);
    state = KernelServerEntityState{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    // A template-less player is kNone and never moves, so the aim is what
    // shows the queued input reached the entity on this tick.
    require(state.aim_direction.z == 1.0f);

    Kernel_Destroy(kernel);
}

// The floor plus four per networked entity: the one player here adds four.
void queue_capacity_rejects_after_capacity() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7794));

    std::uint32_t net_id = 0;
    KernelServerEntityCreateInfo create_info = player_create_info();
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));

    for (std::size_t index = 0; index < 2048 + 4; ++index) {
        KernelVec3 velocity{0.0f, 0.0f, 0.0f};
        require(Kernel_ServerEnqueueEntityVelocity(
            kernel,
            KernelCommandSource_Test,
            net_id,
            &velocity));
    }
    KernelVec3 velocity{1.0f, 0.0f, 0.0f};
    require(!Kernel_ServerEnqueueEntityVelocity(
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
        require(engine.server_enqueue_entity_velocity(
            KernelCommandSource_Test,
            77,
            velocity));
    }

    require(engine.command_queue_.size() == 1600);
    require(engine.command_queue_capacity_warning_count_ == 1);
}

// A 1300-agent world sends an input and a state per agent every tick, plus a
// transform for the ones turning: about 2650 commands, which the fixed 2048
// used to cut short. Three per entity all fit now.
void queue_capacity_grows_with_the_world() {
    network_example::KernelEngine engine(server_config());
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    entt::registry& registry = engine.world_.registry();
    for (std::size_t index = 0; index < 1300; ++index) {
        registry.emplace<network_example::NetworkIdentity>(registry.create());
    }
    require(engine.simulation_command_capacity() == 2048 + 4 * 1300);

    KernelVec3 velocity{0.0f, 0.0f, 0.0f};
    for (std::size_t index = 0; index < 3 * 1300; ++index) {
        require(engine.server_enqueue_entity_velocity(
            KernelCommandSource_Test,
            77,
            velocity));
    }
    require(engine.rejected_simulation_command_count_ == 0);
    require(engine.command_queue_capacity_warning_count_ == 0);
}

void tick_monitor_records_warning_when_threshold_is_low() {
    network_example::KernelEngine engine(server_config());
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    engine.simulation_tick_cost_warning_threshold_us_ = 1;

    engine.simulate_tick();

    require(engine.last_simulation_tick_cost_us_ >= 1);
    require(engine.simulation_tick_cost_warning_count_ == 1);
}

}  // namespace

int main() {
    queued_transform_applies_on_next_tick();
    queued_lifecycle_destroy_applies_on_next_tick();
    queued_rotation_keeps_the_controller_anchor();
    queued_submit_player_input_drains_before_movement();
    queue_capacity_rejects_after_capacity();
    queue_warning_threshold_records_warning_once();
    queue_capacity_grows_with_the_world();
    tick_monitor_records_warning_when_threshold_is_low();
    return 0;
}
