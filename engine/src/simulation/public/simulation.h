#ifndef SIMULATION_PUBLIC_SIMULATION_H_
#define SIMULATION_PUBLIC_SIMULATION_H_

#include <cstdint>
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

class DamagePipeline {
public:
    static constexpr std::uint64_t kGraceWindowUs = 100000;
    static constexpr std::uint64_t kDefensiveActionWindowUs = 100000;

    void clear();
    void ingest_defensive_input(
        PeerId owner_peer,
        const PlayerInput& input,
        std::uint64_t received_server_time_us);
    bool submit_hit(
        const World& world,
        NetId target_net_id,
        NetId source_net_id,
        PeerId source_peer,
        std::uint8_t source_code,
        std::uint16_t damage,
        std::uint64_t hit_time_us);
    void confirm_ready(
        World& world,
        std::uint64_t server_time_us,
        std::uint32_t current_tick,
        std::vector<KernelEvent>* events);
    std::uint32_t pending_count() const;

private:
    enum class DefensiveActionType {
        kDodge,
        kParry,
    };

    struct DefensiveAction {
        PeerId owner_peer = 0;
        DefensiveActionType type = DefensiveActionType::kDodge;
        std::uint64_t action_time_us = 0;
    };

    struct PendingDamage {
        NetId target_net_id = 0;
        PeerId target_peer = 0;
        NetId source_net_id = 0;
        PeerId source_peer = 0;
        std::uint8_t source_code = 0;
        std::uint16_t damage = 0;
        std::uint64_t hit_time_us = 0;
        std::uint64_t confirm_time_us = 0;
        bool canceled = false;
        bool parry_applied = false;
    };

    void apply_defensive_actions(PendingDamage* pending);
    void prune_defensive_actions(std::uint64_t server_time_us);

    std::vector<DefensiveAction> defensive_actions_;
    std::vector<PendingDamage> pending_damage_;
};

void simulate_player_movement(
    World& world,
    const std::vector<QueuedInput>& inputs,
    float fixed_delta_seconds);

void simulate_velocity_movement(World& world, float fixed_delta_seconds);

void simulate_projectiles(World& world, float fixed_delta_seconds);
void simulate_projectiles(
    World& world,
    float fixed_delta_seconds,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events);
void simulate_projectiles(
    World& world,
    float fixed_delta_seconds,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events,
    DamagePipeline* damage_pipeline);

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
