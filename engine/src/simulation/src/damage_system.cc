#include "simulation/public/simulation.h"

namespace network_example {

void apply_damage_applications(
    World& world,
    const std::vector<ConfirmedDamage>& damage_applications,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events) {
    for (const ConfirmedDamage& damage : damage_applications) {
        if (!world.apply_damage(damage.target_net_id, damage.damage)) {
            continue;
        }
        if (events == nullptr) {
            continue;
        }
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
    }
}

void destroy_dead_entities(
    World& world,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events) {
    std::vector<NetId> dead_entities;
    auto view = world.registry().view<NetworkIdentity, Health>();
    for (const entt::entity entity : view) {
        const Health& health = view.get<Health>(entity);
        if (health.max_hp > 0 && health.hp == 0 &&
            !world.registry().all_of<PlayerTag>(entity)) {
            dead_entities.push_back(view.get<NetworkIdentity>(entity).net_id);
        }
    }

    for (NetId net_id : dead_entities) {
        if (world.destroy(net_id) && events != nullptr) {
            events->push_back(
                KernelEvent{KernelEventType_EntityDestroyed, current_tick, net_id, 0, 0});
        }
    }
}

}  // namespace network_example
