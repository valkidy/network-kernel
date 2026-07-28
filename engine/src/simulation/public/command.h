#ifndef SIMULATION_PUBLIC_COMMAND_H_
#define SIMULATION_PUBLIC_COMMAND_H_

#include <cstddef>
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
    kSetEntityHealth,
    kActivateEntity,
    kCreateInventoryContainer,
    kCreateInventoryItem,
    kCreateWorldItem,
    kSubmitGameplayRequest,
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

struct ActivateEntity {
    KernelServerEntityActivateInfo activate_info{};
};

struct CreateInventoryContainer {
    std::uint32_t owner_entity_id = 0;
    std::uint32_t slot_capacity = 0;
};

struct CreateInventoryItem {
    std::uint32_t item_template_id = 0;
    std::uint32_t quantity = 0;
    KernelInventoryContainerId container_id = 0;
};

struct CreateWorldItem {
    std::uint32_t item_template_id = 0;
    std::uint32_t quantity = 0;
    KernelVec3 position{};
};

struct SubmitGameplayRequest {
    KernelGameplayRequest request{};
};

struct Command {
    CommandId id = CommandId::kUnknown;
    CommandSource source = CommandSource::kInternal;
    std::uint64_t completion_token = 0;
    CreateEntity create_entity{};
    DestroyEntity destroy_entity{};
    SubmitInput submit_input{};
    SetEntityTransform set_entity_transform{};
    SetEntityVelocity set_entity_velocity{};
    SetEntityState set_entity_state{};
    SetEntityActorTemplate set_entity_actor_template{};
    ActivateEntity activate_entity{};
    CreateInventoryContainer create_inventory_container{};
    CreateInventoryItem create_inventory_item{};
    CreateWorldItem create_world_item{};
    SubmitGameplayRequest submit_gameplay_request{};
};

struct CommandResult {
    bool ok = false;
    NetId net_id = 0;
    KernelInventoryContainerId inventory_container_id = 0;
    KernelItemInstanceId item_instance_id = 0;
};

class CommandQueue {
public:
    static constexpr std::size_t kDefaultCapacity = 2048;
    static constexpr std::size_t kWarningThreshold = 1536;

    bool enqueue(
        const Command& command,
        std::size_t capacity = kDefaultCapacity) {
        if (commands_.size() >= capacity) {
            return false;
        }
        commands_.push_back(command);
        return true;
    }

    std::span<const Command> commands() const {
        return commands_;
    }

    std::size_t size() const {
        return commands_.size();
    }

    void clear() {
        commands_.clear();
    }

private:
    std::vector<Command> commands_;
};

}  // namespace network_example::simulation

#endif  // SIMULATION_PUBLIC_COMMAND_H_
