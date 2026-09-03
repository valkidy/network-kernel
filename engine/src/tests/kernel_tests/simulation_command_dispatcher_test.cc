#include <cstdio>
#include <cstdlib>
#include <cstdint>

#include <entt/entt.hpp>

#include "kernel/public/kernel_api.h"
#include "simulation/public/command.h"

#define private public
#include "kernel/src/kernel.h"
#include "simulation/src/command_dispatcher.h"

namespace {

// assert() is compiled out under -c opt, which is the configuration this suite
// runs in, so every check in this file was previously not being run at all.
// This is the fourth file in this area found that way.
void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

}  // namespace

#undef private

namespace {

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

KernelConfig server_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    return config;
}

void reset_running_server(network_example::KernelEngine* engine) {
    engine->reset_runtime_state(KernelMode_DedicatedServer);
}

void dispatcher_routes_create_and_destroy_to_lifecycle_system() {
    network_example::KernelEngine engine(server_config());
    reset_running_server(&engine);
    network_example::simulation::Dispatcher dispatcher;

    network_example::simulation::Command create{};
    create.id = network_example::simulation::CommandId::kCreateEntity;
    create.source = network_example::simulation::CommandSource::kTest;
    create.create_entity.create_info = player_create_info();

    const network_example::simulation::CommandResult create_result =
        dispatcher.dispatch(engine, create);
    require(create_result.ok);
    require(create_result.net_id != 0);
    require(engine.world_.find_entity(create_result.net_id).has_value());

    network_example::simulation::Command destroy{};
    destroy.id = network_example::simulation::CommandId::kDestroyEntity;
    destroy.source = network_example::simulation::CommandSource::kTest;
    destroy.destroy_entity.net_id = create_result.net_id;
    destroy.destroy_entity.reason = KernelDespawnReason_Destroyed;

    const network_example::simulation::CommandResult destroy_result =
        dispatcher.dispatch(engine, destroy);
    require(destroy_result.ok);
    require(!engine.world_.find_entity(create_result.net_id).has_value());
}

void dispatcher_routes_submit_player_input_to_movement_system() {
    network_example::KernelEngine engine(server_config());
    reset_running_server(&engine);
    std::uint32_t net_id = 0;
    require(engine.server_create_entity(player_create_info(), &net_id));

    KernelPlayerInput input{};
    input.input_seq = 3;
    input.move = KernelVec2{1.0f, 0.0f};

    network_example::simulation::Command command{};
    command.id = network_example::simulation::CommandId::kSubmitPlayerInput;
    command.source = network_example::simulation::CommandSource::kTest;
    command.submit_player_input.net_id = net_id;
    command.submit_player_input.input = input;

    network_example::simulation::Dispatcher dispatcher;
    const network_example::simulation::CommandResult result =
        dispatcher.dispatch(engine, command);
    require(result.ok);
    require(engine.pending_inputs_.size() == 1);
    require(engine.pending_inputs_[0].controlled_net_id == net_id);
    require(engine.pending_inputs_[0].input.input_seq == 3);
}

}  // namespace

int main() {
    dispatcher_routes_create_and_destroy_to_lifecycle_system();
    dispatcher_routes_submit_player_input_to_movement_system();
    return 0;
}
