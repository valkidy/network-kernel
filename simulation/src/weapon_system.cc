#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace network_example {
namespace {

glm::vec3 input_aim_to_world(const PlayerInput& input) {
    glm::vec3 aim{input.aim_dir.x, input.aim_dir.y, input.aim_dir.z};
    if (glm::length(aim) <= 0.0001f) {
        return glm::vec3{1.0f, 0.0f, 0.0f};
    }
    return glm::normalize(aim);
}

void push_event(
    std::vector<KernelEvent>* events,
    KernelEventType type,
    std::uint32_t tick,
    NetId net_id,
    PeerId peer_id,
    std::uint32_t code = 0) {
    if (events == nullptr) {
        return;
    }
    events->push_back(KernelEvent{type, tick, net_id, peer_id, code});
}

}  // namespace

bool ray_intersects_aabb(
    const glm::vec3& ray_origin,
    const glm::vec3& ray_direction,
    const glm::vec3& box_center,
    const glm::vec3& box_half_extents,
    float* out_distance) {
    float t_min = 0.0f;
    float t_max = std::numeric_limits<float>::max();
    const glm::vec3 min_corner = box_center - box_half_extents;
    const glm::vec3 max_corner = box_center + box_half_extents;

    for (int axis = 0; axis < 3; ++axis) {
        const float origin = ray_origin[axis];
        const float direction = ray_direction[axis];
        if (std::abs(direction) < 0.000001f) {
            if (origin < min_corner[axis] || origin > max_corner[axis]) {
                return false;
            }
            continue;
        }

        float t1 = (min_corner[axis] - origin) / direction;
        float t2 = (max_corner[axis] - origin) / direction;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        t_min = std::max(t_min, t1);
        t_max = std::min(t_max, t2);
        if (t_min > t_max) {
            return false;
        }
    }

    if (out_distance != nullptr) {
        *out_distance = t_min;
    }
    return true;
}

void simulate_hitscan_weapons(
    World& world,
    const std::vector<QueuedInput>& inputs,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events) {
    constexpr std::uint16_t kRifleDamage = 25;
    constexpr float kMaxRange = 100.0f;

    for (const QueuedInput& queued_input : inputs) {
        if ((queued_input.input.buttons & InputButton_Fire) == 0) {
            continue;
        }

        auto player_view = world.registry().view<NetworkIdentity, Transform, WeaponState, PlayerTag>();
        for (const entt::entity player_entity : player_view) {
            const NetworkIdentity& player_identity = player_view.get<NetworkIdentity>(player_entity);
            if (player_identity.owner_peer != queued_input.owner_peer) {
                continue;
            }

            WeaponState& weapon = player_view.get<WeaponState>(player_entity);
            if (current_tick < weapon.next_fire_tick || weapon.ammo == 0) {
                continue;
            }
            weapon.next_fire_tick = current_tick + 3;
            --weapon.ammo;
            push_event(
                events,
                KernelEventType_FireConfirmed,
                current_tick,
                player_identity.net_id,
                queued_input.owner_peer);

            const Transform& player_transform = player_view.get<Transform>(player_entity);
            const glm::vec3 origin = player_transform.position + glm::vec3{0.0f, 1.0f, 0.0f};
            const glm::vec3 direction = input_aim_to_world(queued_input.input);

            float best_distance = kMaxRange;
            NetId best_net_id = 0;
            auto target_view =
                world.registry().view<NetworkIdentity, Transform, Health, Hitbox>();
            for (const entt::entity target_entity : target_view) {
                const NetworkIdentity& target_identity =
                    target_view.get<NetworkIdentity>(target_entity);
                if (target_identity.net_id == player_identity.net_id) {
                    continue;
                }
                const Transform& target_transform = target_view.get<Transform>(target_entity);
                const Hitbox& hitbox = target_view.get<Hitbox>(target_entity);
                float distance = 0.0f;
                if (ray_intersects_aabb(
                        origin,
                        direction,
                        target_transform.position + hitbox.center,
                        hitbox.half_extents,
                        &distance) &&
                    distance < best_distance) {
                    best_distance = distance;
                    best_net_id = target_identity.net_id;
                }
            }

            if (best_net_id != 0 && world.apply_damage(best_net_id, kRifleDamage)) {
                push_event(
                    events,
                    KernelEventType_HitConfirmed,
                    current_tick,
                    best_net_id,
                    queued_input.owner_peer);
                push_event(
                    events,
                    KernelEventType_DamageApplied,
                    current_tick,
                    best_net_id,
                    queued_input.owner_peer,
                    kRifleDamage);
            }
        }
    }
}

}  // namespace network_example
