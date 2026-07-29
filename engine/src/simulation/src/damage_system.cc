#include "simulation/public/simulation.h"

#include <optional>

namespace network_example {

std::vector<ConfirmedDamage> apply_damage_applications(
    World& world,
    const std::vector<ConfirmedDamage>& damage_applications,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events) {
    std::vector<ConfirmedDamage> health_depleted;
    for (const ConfirmedDamage& damage : damage_applications) {
        const std::optional<entt::entity> target =
            world.find_entity(damage.target_net_id);
        if (!target.has_value() ||
            !world.registry().all_of<Health>(*target)) {
            continue;
        }
        const std::uint16_t hp_before =
            world.registry().get<Health>(*target).hp;
        if (!world.apply_damage(damage.target_net_id, damage.damage)) {
            continue;
        }
        if (hp_before > 0u && world.registry().get<Health>(*target).hp == 0u) {
            health_depleted.push_back(damage);
        }
        if (events != nullptr) {
            events->push_back(KernelEvent{
                KernelEventType_HitConfirmed,
                current_tick,
                damage.target_net_id,
                damage.source_peer,
                damage.source_code,
                damage.hit_time_us,
                damage.hit_time_us,
            });
            events->push_back(KernelEvent{
                KernelEventType_DamageApplied,
                current_tick,
                damage.target_net_id,
                damage.source_peer,
                damage.damage,
                damage.hit_time_us,
                damage.hit_time_us,
            });
            const std::uint16_t hp_after =
                world.registry().get<Health>(*target).hp;
            if (hp_after < hp_before) {
                events->push_back(KernelEvent{
                    KernelEventType_HealthChanged,
                    current_tick,
                    damage.target_net_id,
                    damage.source_peer,
                    0u,
                    damage.hit_time_us,
                    damage.hit_time_us,
                    -static_cast<std::int32_t>(hp_before - hp_after),
                });
            }
        }
    }
    return health_depleted;
}

}  // namespace network_example
