#ifndef SIMULATION_PUBLIC_COMMAND_H_
#define SIMULATION_PUBLIC_COMMAND_H_

#include <cstdint>
#include <span>
#include <vector>

#include "kernel/public/kernel_types.h"
#include "world/public/components.h"

namespace network_example::simulation {

enum class CommandId : std::uint8_t {
    kUnknown = 0,
    kCreateEntity,
    kDestroyEntity,
    kSubmitInput,
    kSetEntityTransform,
    kSetEntityVelocity,
    kSetEntityState,
    kSetEntityActorTemplate,
};

enum class CommandSource : std::uint8_t {
    kInternal = 0,
    kPlayerInput,
    kAi,
    kControlPlane,
    kTest,
};

struct CreateEntity {
    KernelServerEntityCreateInfo create_info{};
};

struct DestroyEntity {
    NetId net_id = 0;
    std::uint32_t reason = 0;
};

struct SubmitInput {
    NetId net_id = 0;
    PlayerInput input{};
};

struct SetEntityTransform {
    NetId net_id = 0;
    KernelVec3 position{};
    KernelQuat rotation{};
};

struct SetEntityVelocity {
    NetId net_id = 0;
    KernelVec3 velocity{};
};

struct SetEntityState {
    NetId net_id = 0;
    std::uint16_t animation_state = 0;
    std::uint32_t visual_flags = 0;
};

struct SetEntityActorTemplate {
    NetId net_id = 0;
    std::uint32_t actor_template_id = 0;
};

struct Command {
    CommandId id = CommandId::kUnknown;
    CommandSource source = CommandSource::kInternal;
    CreateEntity create_entity{};
    DestroyEntity destroy_entity{};
    SubmitInput submit_input{};
    SetEntityTransform set_entity_transform{};
    SetEntityVelocity set_entity_velocity{};
    SetEntityState set_entity_state{};
    SetEntityActorTemplate set_entity_actor_template{};
};

struct CommandResult {
    bool ok = false;
    NetId net_id = 0;
};

class CommandQueue {
public:
    void enqueue(const Command& command) {
        commands_.push_back(command);
    }

    std::span<const Command> commands() const {
        return commands_;
    }

    void clear() {
        commands_.clear();
    }

private:
    std::vector<Command> commands_;
};

}  // namespace network_example::simulation

#endif  // SIMULATION_PUBLIC_COMMAND_H_
