#include "simulation/public/simulation.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace network_example {
namespace {

bool is_player_entity(const World& world, entt::entity entity) {
    return world.registry().all_of<PlayerTag>(entity);
}

bool action_overlaps_hit(
    std::uint64_t action_time_us,
    std::uint64_t hit_time_us) {
    return action_time_us <= hit_time_us &&
           hit_time_us <=
               action_time_us + DamagePipeline::kDefensiveActionWindowUs;
}

std::uint16_t parry_reduced_damage(std::uint16_t damage) {
    if (damage == 0) {
        return 0;
    }
    return static_cast<std::uint16_t>(
        std::max(1.0f, std::round(static_cast<float>(damage) * 0.5f)));
}

}  // namespace

void DamagePipeline::clear() {
    defensive_actions_.clear();
    queued_damage_.clear();
    pending_damage_.clear();
}

void DamagePipeline::ingest_defensive_input(
    PeerId owner_peer,
    const PlayerInput& input,
    std::uint64_t received_server_time_us,
    std::uint64_t action_server_time_us,
    bool has_action_server_time) {
    if (owner_peer == 0) {
        return;
    }

    const std::uint64_t action_time_us =
        has_action_server_time
            ? action_server_time_us
            : (input.client_action_time_us == 0 ? received_server_time_us
                                                : input.client_action_time_us);

    if ((input.buttons & InputButton_Dodge) != 0) {
        defensive_actions_.push_back(
            DefensiveAction{owner_peer, DefensiveActionType::kDodge, action_time_us});
    }
    if ((input.buttons & InputButton_Parry) != 0) {
        defensive_actions_.push_back(
            DefensiveAction{owner_peer, DefensiveActionType::kParry, action_time_us});
    }

    for (PendingDamage& pending : pending_damage_) {
        apply_defensive_actions(&pending);
    }
    prune_defensive_actions(received_server_time_us);
}

bool DamagePipeline::submit_hit(
    const World& world,
    NetId target_net_id,
    NetId source_net_id,
    PeerId source_peer,
    std::uint8_t source_code,
    std::uint16_t damage,
    std::uint64_t hit_time_us) {
    (void)world;
    return submit_damage_request(DamageRequest{
        0,
        0,
        source_net_id,
        target_net_id,
        source_peer,
        source_code,
        damage,
        hit_time_us,
        glm::vec3{0.0f, 0.0f, 0.0f},
    });
}

bool DamagePipeline::submit_damage_request(const DamageRequest& request) {
    if (request.damage == 0 || request.target_net_id == 0) {
        return false;
    }
    queued_damage_.push_back(request);
    return true;
}

void DamagePipeline::confirm_ready(
    World& world,
    std::uint64_t server_time_us,
    std::uint32_t current_tick,
    std::vector<KernelEvent>* events) {
    for (const DamageRequest& request : queued_damage_) {
        const std::optional<entt::entity> target_entity =
            world.find_entity(request.target_net_id);
        if (!target_entity.has_value() ||
            !world.registry().all_of<NetworkIdentity, Health>(*target_entity)) {
            continue;
        }

        const NetworkIdentity& target_identity =
            world.registry().get<NetworkIdentity>(*target_entity);
        const bool delay_for_defense =
            request.source_peer == 0 && is_player_entity(world, *target_entity);
        PendingDamage pending{
            request.target_net_id,
            target_identity.owner_peer,
            request.source_net_id,
            request.source_peer,
            request.source_code,
            request.damage,
            request.hit_time_us,
            delay_for_defense
                ? request.hit_time_us + DamagePipeline::kGraceWindowUs
                : request.hit_time_us,
            request.server_tick,
            request.sequence_id,
            false,
            false,
        };
        apply_defensive_actions(&pending);
        pending_damage_.push_back(pending);
    }
    queued_damage_.clear();

    for (PendingDamage& pending : pending_damage_) {
        apply_defensive_actions(&pending);
    }

    std::stable_sort(
        pending_damage_.begin(),
        pending_damage_.end(),
        [](const PendingDamage& lhs, const PendingDamage& rhs) {
            if (lhs.server_tick != rhs.server_tick) {
                return lhs.server_tick < rhs.server_tick;
            }
            if (lhs.sequence_id != rhs.sequence_id) {
                return lhs.sequence_id < rhs.sequence_id;
            }
            if (lhs.source_net_id != rhs.source_net_id) {
                return lhs.source_net_id < rhs.source_net_id;
            }
            if (lhs.target_net_id != rhs.target_net_id) {
                return lhs.target_net_id < rhs.target_net_id;
            }
            return lhs.confirm_time_us < rhs.confirm_time_us;
        });

    auto ready_begin = std::remove_if(
        pending_damage_.begin(),
        pending_damage_.end(),
        [&](const PendingDamage& pending) {
            if (server_time_us < pending.confirm_time_us) {
                return false;
            }
            if (pending.canceled || pending.damage == 0) {
                return true;
            }
            if (world.apply_damage(pending.target_net_id, pending.damage)) {
                if (events != nullptr) {
                    events->push_back(KernelEvent{
                        KernelEventType_HitConfirmed,
                        current_tick,
                        pending.target_net_id,
                        pending.source_peer,
                        pending.source_code,
                        pending.hit_time_us,
                        pending.hit_time_us,
                    });
                    events->push_back(KernelEvent{
                        KernelEventType_DamageApplied,
                        current_tick,
                        pending.target_net_id,
                        pending.source_peer,
                        pending.damage,
                        pending.hit_time_us,
                        pending.hit_time_us,
                    });
                }
            }
            return true;
        });
    pending_damage_.erase(ready_begin, pending_damage_.end());
    prune_defensive_actions(server_time_us);
}

std::uint32_t DamagePipeline::pending_count() const {
    return static_cast<std::uint32_t>(
        pending_damage_.size() + queued_damage_.size());
}

void DamagePipeline::apply_defensive_actions(PendingDamage* pending) {
    if (pending == nullptr || pending->canceled) {
        return;
    }

    for (const DefensiveAction& action : defensive_actions_) {
        if (action.owner_peer == pending->target_peer &&
            action.type == DefensiveActionType::kDodge &&
            action_overlaps_hit(action.action_time_us, pending->hit_time_us)) {
            pending->canceled = true;
            return;
        }
    }

    if (pending->parry_applied) {
        return;
    }
    for (const DefensiveAction& action : defensive_actions_) {
        if (action.owner_peer == pending->target_peer &&
            action.type == DefensiveActionType::kParry &&
            action_overlaps_hit(action.action_time_us, pending->hit_time_us)) {
            pending->damage = parry_reduced_damage(pending->damage);
            pending->parry_applied = true;
            return;
        }
    }
}

void DamagePipeline::prune_defensive_actions(std::uint64_t server_time_us) {
    defensive_actions_.erase(
        std::remove_if(
            defensive_actions_.begin(),
            defensive_actions_.end(),
            [server_time_us](const DefensiveAction& action) {
                return action.action_time_us +
                           DamagePipeline::kDefensiveActionWindowUs <
                       server_time_us;
            }),
        defensive_actions_.end());
}

}  // namespace network_example
