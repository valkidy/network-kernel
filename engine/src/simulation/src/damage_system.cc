#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace network_example {

bool damage_source_may_damage(
    const World& world,
    std::uint32_t attacker_collision_mask,
    NetId target_net_id) {
    if (target_net_id == 0) {
        return false;
    }
    const std::optional<entt::entity> target = world.find_entity(target_net_id);
    if (!target.has_value() || !world.registry().all_of<Health>(*target)) {
        return false;
    }
    const GameplaySide* side = world.registry().try_get<GameplaySide>(*target);
    return side == nullptr || (attacker_collision_mask & side->category) != 0u;
}

bool apply_stagger(
    World& world,
    NetId target_net_id,
    std::uint16_t damage,
    float explicit_stagger,
    PeerId source_peer,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events) {
    const std::optional<entt::entity> target = world.find_entity(target_net_id);
    if (!target.has_value()) {
        return false;
    }
    const StaggerProfile* profile =
        world.registry().try_get<StaggerProfile>(*target);
    if (profile == nullptr || !(profile->threshold > 0.0f)) {
        return false;
    }
    if (world.registry().all_of<Health>(*target) &&
        world.registry().get<Health>(*target).hp == 0u) {
        return false;
    }
    const float amount = explicit_stagger >= 0.0f
        ? explicit_stagger
        : static_cast<float>(damage) * profile->stagger_per_damage;
    if (!std::isfinite(amount) || amount <= 0.0f) {
        return false;
    }
    StaggerState& state = world.registry().get_or_emplace<StaggerState>(*target);
    if (current_tick < state.until_tick || current_tick < state.immune_until_tick) {
        return false;
    }
    // Decay is settled lazily here rather than by a per-tick pass: the meter
    // is only ever read when a hit lands.
    const std::uint32_t idle_ticks = current_tick - state.last_hit_tick;
    if (state.meter > 0.0f && idle_ticks > profile->decay_delay_ticks) {
        state.meter = std::max(
            0.0f,
            state.meter - profile->decay_per_tick *
                static_cast<float>(idle_ticks - profile->decay_delay_ticks));
    }
    state.meter += amount;
    state.last_hit_tick = current_tick;
    if (state.meter < profile->threshold) {
        return false;
    }
    state.meter = 0.0f;
    // Damage lands at the end of the tick, after the action pass, so the
    // first tick the target can feel this is the next one. The +1 makes
    // stagger_ticks the number of ticks the target is actually held.
    state.until_tick = current_tick + 1u + profile->stagger_ticks;
    state.immune_until_tick = state.until_tick + profile->immunity_ticks;
    ++state.trigger_count;
    world.registry().get_or_emplace<ReplicationState>(*target).visual_flags |=
        kVisualFlagStaggered;
    if (events != nullptr) {
        events->push_back(KernelEvent{
            KernelEventType_Staggered,
            current_tick,
            target_net_id,
            source_peer,
            profile->stagger_ticks,
        });
    }
    return true;
}

bool is_staggered(const World& world, entt::entity entity, std::uint32_t current_tick) {
    const StaggerState* state = world.registry().try_get<StaggerState>(entity);
    return state != nullptr && current_tick < state->until_tick;
}

KernelLocalActionResultReason action_block_reason(
    const World& world,
    entt::entity entity,
    std::uint32_t current_tick) {
    if (is_staggered(world, entity, current_tick)) {
        return KernelLocalActionResultReason_Staggered;
    }
    const ImpulseLockout* lockout = world.registry().try_get<ImpulseLockout>(entity);
    if (lockout != nullptr && current_tick < lockout->until_tick) {
        return KernelLocalActionResultReason_KnockedBack;
    }
    return KernelLocalActionResultReason_None;
}

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
        if (const DamageImmunity* immunity =
                world.registry().try_get<DamageImmunity>(*target);
            immunity != nullptr && current_tick < immunity->until_tick) {
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
        // Every damage path funnels through here -- this is the only caller
        // of World::apply_damage -- so this is the one place a hit can stagger.
        (void)apply_stagger(
            world,
            damage.target_net_id,
            damage.damage,
            damage.stagger,
            damage.source_peer,
            current_tick,
            events);
    }
    return health_depleted;
}

}  // namespace network_example
