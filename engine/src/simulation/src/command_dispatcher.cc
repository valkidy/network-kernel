#include "simulation/src/command_dispatcher.h"

#include "kernel/src/kernel.h"
#include "simulation/src/systems.h"

namespace network_example::simulation {

CommandResult Dispatcher::dispatch(
    KernelEngine& engine,
    const Command& command) const {
    switch (command.id) {
        case CommandId::kCreateEntity: {
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
        case CommandId::kSubmitInput:
            return CommandResult{
                MovementSystem{}.submit_input(
                    engine,
                    command.submit_input.net_id,
                    command.submit_input.input),
                command.submit_input.net_id};
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
        case CommandId::kUnknown:
            break;
    }
    return CommandResult{};
}

}  // namespace network_example::simulation
