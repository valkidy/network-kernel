#ifndef SIMULATION_PUBLIC_SIMULATION_H_
#define SIMULATION_PUBLIC_SIMULATION_H_

#include <vector>

#include "kernel/public/kernel_types.h"
#include "sync/public/history_buffer.h"
#include "world/public/world.h"

namespace network_example {

struct QueuedInput {
    PeerId owner_peer = 0;
    PlayerInput input{};
    std::uint32_t received_server_tick = 0;
};

void simulate_player_movement(
    World& world,
    const std::vector<QueuedInput>& inputs,
    float fixed_delta_seconds);

void simulate_projectiles(World& world, float fixed_delta_seconds);
void simulate_projectiles(
    World& world,
    float fixed_delta_seconds,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events);

bool ray_intersects_aabb(
    const glm::vec3& ray_origin,
    const glm::vec3& ray_direction,
    const glm::vec3& box_center,
    const glm::vec3& box_half_extents,
    float* out_distance);

void simulate_hitscan_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events);
void simulate_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events);
void simulate_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    const HistoryFrame* rewind_frame);

void destroy_dead_entities(
    World& world,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events);

}  // namespace network_example

#endif  // SIMULATION_PUBLIC_SIMULATION_H_
