#ifndef SIMULATION_SRC_SYSTEMS_H_
#define SIMULATION_SRC_SYSTEMS_H_

#include <cstdint>

#include "kernel/public/kernel_types.h"
#include "world/public/components.h"

namespace network_example {

class KernelEngine;

class EntityLifecycleSystem {
public:
    bool create_entity(
        KernelEngine& engine,
        const KernelServerEntityCreateInfo& create_info,
        NetId* out_net_id) const;

    bool destroy_entity(
        KernelEngine& engine,
        NetId net_id,
        std::uint32_t reason) const;
};

class EntityStateSystem {
public:
    bool set_actor_template(
        KernelEngine& engine,
        NetId net_id,
        std::uint32_t actor_template_id) const;
    bool set_transform(
        KernelEngine& engine,
        NetId net_id,
        const KernelVec3& position,
        const KernelQuat& rotation) const;
    bool set_velocity(
        KernelEngine& engine,
        NetId net_id,
        const KernelVec3& velocity) const;
    bool set_state(
        KernelEngine& engine,
        NetId net_id,
        std::uint16_t animation_state,
        std::uint32_t visual_flags) const;
};

class MovementSystem {
public:
    bool submit_input(
        KernelEngine& engine,
        NetId net_id,
        const PlayerInput& input) const;
};

}  // namespace network_example

#endif  // SIMULATION_SRC_SYSTEMS_H_
