#include "simulation/src/command_dispatcher.h"

#include "kernel/src/kernel.h"
#include "simulation/src/systems.h"

namespace network_example::simulation {

CommandResult Dispatcher::dispatch(
    KernelEngine& engine,
    const Command& command) const {
    switch (command.id) {
        case CommandId::kCreateEntity: {
            // Group membership and its bookkeeping used to be attached here,
            // for the game-rule director. Both directors are game_server's now,
            // and game_server spawns synchronously, so it holds the net ids it
            // created and needs nothing recorded on its behalf.
            NetId net_id = 0;
            const bool ok = EntityLifecycleSystem{}.create_entity(
                engine,
                command.create_entity.create_info,
                &net_id);
            return CommandResult{ok, net_id};
        }
        case CommandId::kDestroyEntity:
            return CommandResult{
                EntityLifecycleSystem{}.destroy_entity(
                    engine,
                    command.destroy_entity.net_id,
                    command.destroy_entity.reason),
                command.destroy_entity.net_id};
        case CommandId::kSubmitPlayerInput:
            return CommandResult{
                MovementSystem{}.submit_player_input(
                    engine,
                    command.submit_player_input.net_id,
                    command.submit_player_input.input),
                command.submit_player_input.net_id};
        case CommandId::kSetEntityTransform:
            return CommandResult{
                EntityStateSystem{}.set_transform(
                    engine,
                    command.set_entity_transform.net_id,
                    command.set_entity_transform.position,
                    command.set_entity_transform.rotation),
                command.set_entity_transform.net_id};
        case CommandId::kSetEntityVelocity:
            return CommandResult{
                EntityStateSystem{}.set_velocity(
                    engine,
                    command.set_entity_velocity.net_id,
                    command.set_entity_velocity.velocity),
                command.set_entity_velocity.net_id};
        case CommandId::kSetEntityState:
            return CommandResult{
                EntityStateSystem{}.set_state(
                    engine,
                    command.set_entity_state.net_id,
                    command.set_entity_state.animation_state,
                    command.set_entity_state.visual_flags),
                command.set_entity_state.net_id};
        case CommandId::kSetEntityHealth:
            return CommandResult{
                engine.server_set_entity_health(
                    command.set_entity_state.net_id,
                    command.set_entity_state.animation_state),
                command.set_entity_state.net_id};
        case CommandId::kSetEntityActorTemplate:
            return CommandResult{
                EntityStateSystem{}.set_actor_template(
                    engine,
                    command.set_entity_actor_template.net_id,
                    command.set_entity_actor_template.actor_template_id),
                command.set_entity_actor_template.net_id};
        case CommandId::kActivateEntity:
            return CommandResult{
                ActivationSystem{}.activate_entity(
                    engine,
                    command.activate_entity.activate_info),
                command.activate_entity.activate_info.subject_net_id};
        case CommandId::kCreateInventoryContainer: {
            KernelInventoryContainerId container_id = 0;
            const bool ok = engine.server_create_inventory_container(
                command.create_inventory_container.owner_entity_id,
                command.create_inventory_container.slot_capacity,
                &container_id);
            return CommandResult{ok, 0u, container_id, 0u};
        }
        case CommandId::kCreateInventoryItem: {
            KernelItemInstanceId item_id = 0;
            const bool ok = engine.server_create_inventory_item(
                command.create_inventory_item.item_template_id,
                command.create_inventory_item.quantity,
                command.create_inventory_item.container_id,
                &item_id);
            return CommandResult{ok, 0u, 0u, item_id};
        }
        case CommandId::kCreateWorldItem: {
            KernelItemInstanceId item_id = 0;
            NetId prop_id = 0;
            const bool ok = engine.server_create_world_item(
                command.create_world_item.item_template_id,
                command.create_world_item.quantity,
                command.create_world_item.position,
                &item_id,
                &prop_id);
            return CommandResult{ok, prop_id, 0u, item_id};
        }
        case CommandId::kSubmitGameplayRequest:
            return CommandResult{
                engine.server_submit_gameplay_request(
                    command.submit_gameplay_request.request)};
        case CommandId::kUnknown:
            break;
    }
    return CommandResult{};
}

}  // namespace network_example::simulation
