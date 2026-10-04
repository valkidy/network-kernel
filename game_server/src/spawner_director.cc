#include "game_server/src/spawner_director.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <spdlog/spdlog.h>

#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
constexpr std::size_t kInitialQueriedEntities = 64;
constexpr std::size_t kMaxQueriedEntities = 8192;

// An authored offset, turned by the carrier's rotation and placed where the
// carrier is standing. Written out rather than pulled from glm because this is
// the only rotation game_server does, and the identity case -- every carrier
// placed unrotated today -- has to cost nothing.
KernelVec3 carrier_world_point(
    const KernelServerEntityState& carrier,
    const KernelVec3& local) {
    const float x = carrier.rotation.x;
    const float y = carrier.rotation.y;
    const float z = carrier.rotation.z;
    const float w = carrier.rotation.w;
    // v + 2 * cross(q.xyz, cross(q.xyz, v) + w * v)
    const float tx = 2.0f * (y * local.z - z * local.y);
    const float ty = 2.0f * (z * local.x - x * local.z);
    const float tz = 2.0f * (x * local.y - y * local.x);
    return KernelVec3{
        carrier.position.x + local.x + w * tx + (y * tz - z * ty),
        carrier.position.y + local.y + w * ty + (z * tx - x * tz),
        carrier.position.z + local.z + w * tz + (x * ty - y * tx),
    };
}

// Whether the kernel still knows this net id at all. Deliberately not a check
// on `valid`: an entity created this tick is not valid until physics finalises
// it, so a ceiling that treated "not yet valid" as "dead" would let a nest put
// out a fresh wave on the tick after every wave, forever. The query returns
// false only when the entity is gone, which is the question being asked.
bool entity_alive(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    return Kernel_ServerGetEntityState(kernel, net_id, &state);
}

}  // namespace

std::string validate_spawner_config(const SpawnerConfig& spawner) {
    const std::string composition_error = validate_spawn_composition(
        spawner.composition, spawner.count_min, spawner.count_max);
    if (!composition_error.empty()) {
        return "spawner " + composition_error;
    }
    if (spawner.radius <= 0.0f) {
        return "spawner radius must be positive";
    }
    if (spawner.interval_ticks == 0u) {
        return "spawner interval must be at least one tick";
    }
    // A ceiling below the smallest wave can never be satisfied, so the spawner
    // would fire once and then be permanently blocked -- which reads as a nest
    // that stopped working rather than as a misconfiguration.
    if (spawner.max_live_agents != 0u &&
        spawner.max_live_agents < spawner.count_max) {
        return "spawner ceiling is below the largest wave it would draw";
    }
    if (!std::isfinite(spawner.offset.x) || !std::isfinite(spawner.offset.y) ||
        !std::isfinite(spawner.offset.z)) {
        return "spawner offset must be finite";
    }
    if (spawner.trigger == SpawnerTrigger::kOnAlert) {
        // A call for help arrives where the fight is, not through a door: the
        // walk-out holds a unit for the walk, which is the wrong moment to be
        // holding it.
        if (spawner.entry.authored) {
            return "spawner trigger on_alert cannot have an entry";
        }
        // Unbounded calls with no ceiling of its own leave only the shared
        // reinforce budget between one carrier and the whole of it.
        if (spawner.calls_per_alert == 0u && spawner.max_live_agents == 0u) {
            return "spawner with unbounded calls_per_alert requires "
                   "max_live_agents";
        }
    }
    if (spawner.entry.authored) {
        if (spawner.entry.exits.empty()) {
            return "spawner entry requires at least one exit";
        }
        if (spawner.entry.max_ticks == 0u) {
            return "spawner entry max_ticks must be at least one tick";
        }
        // Zero is the kernel's "engine default", which here means the carrier
        // goes on blocking the unit it just put inside itself: it would stand in
        // the doorway until the timeout and then be released where it started.
        // An entry that cannot be walked is a misconfiguration, not a style.
        if (spawner.entry.movement_collision_mask == 0u) {
            return "spawner entry movement_collision_mask must name the layers "
                   "that still block a unit on its way out";
        }
        if ((spawner.entry.movement_collision_mask &
             ~static_cast<std::uint32_t>(KERNEL_MOVEMENT_MASK_SUPPORTED)) != 0u) {
            return "spawner entry movement_collision_mask names a movement "
                   "layer the kernel does not have";
        }
    }
    return {};
}

SpawnerDirector::SpawnerDirector(
    std::vector<SpawnerCarrierConfig> carriers,
    std::uint32_t reinforce_max_live_agents)
    : carriers_(std::move(carriers)),
      reinforce_max_live_agents_(reinforce_max_live_agents) {
    for (const SpawnerCarrierConfig& carrier : carriers_) {
        // A caller is found by its own alert signal, never by query: walking
        // every actor each tick to find the few that might call would cost the
        // whole population for nothing.
        if (carrier.spawner.trigger != SpawnerTrigger::kInterval) {
            continue;
        }
        if (std::find(
                queried_entity_types_.begin(),
                queried_entity_types_.end(),
                carrier.entity_type) == queried_entity_types_.end()) {
            queried_entity_types_.push_back(carrier.entity_type);
        }
    }
}

SpawnerTrigger SpawnerDirector::trigger_of(const Instance& instance) const {
    const SpawnerCarrierConfig* carrier = carrier_for(instance.entity_template_id);
    return carrier == nullptr ? SpawnerTrigger::kInterval
                              : carrier->spawner.trigger;
}

bool SpawnerDirector::is_alert_carrier(std::uint32_t entity_template_id) const {
    const SpawnerCarrierConfig* carrier = carrier_for(entity_template_id);
    return carrier != nullptr &&
        carrier->spawner.trigger == SpawnerTrigger::kOnAlert;
}

const SpawnerCarrierConfig* SpawnerDirector::carrier_for(
    std::uint32_t entity_template_id) const {
    const auto found = std::find_if(
        carriers_.begin(),
        carriers_.end(),
        [entity_template_id](const SpawnerCarrierConfig& candidate) {
            return candidate.entity_template_id == entity_template_id;
        });
    return found == carriers_.end() ? nullptr : &*found;
}

void SpawnerDirector::tick(
    KernelHandle* kernel,
    const std::vector<AlertSignal>& alerts) {
    if (kernel == nullptr || carriers_.empty()) {
        return;
    }
    tick_interval_carriers(kernel);
    tick_alert_carriers(kernel, alerts);
}

void SpawnerDirector::tick_interval_carriers(KernelHandle* kernel) {
    if (queried_entity_types_.empty()) {
        return;
    }
    // Discover carriers. Only the entity types some carrier actually uses are
    // queried, so a catalog with no spawners does no work at all and one with
    // only nests never walks the actor list.
    std::vector<std::uint32_t> live_carriers;
    for (const std::uint16_t entity_type : queried_entity_types_) {
        if (query_buffer_.size() < kInitialQueriedEntities) {
            query_buffer_.resize(kInitialQueriedEntities);
        }
        while (true) {
            for (KernelServerEntityState& state : query_buffer_) {
                state.struct_size = sizeof(KernelServerEntityState);
            }
            const std::uint32_t count = Kernel_ServerQueryEntities(
                kernel,
                entity_type,
                query_buffer_.data(),
                static_cast<std::uint32_t>(query_buffer_.size()));
            // The query reports what it wrote, never what it had, so a full
            // buffer is indistinguishable from a truncated one.
            if (count >= query_buffer_.size() &&
                query_buffer_.size() < kMaxQueriedEntities) {
                query_buffer_.resize(query_buffer_.size() * 2);
                continue;
            }
            for (std::uint32_t index = 0; index < count; ++index) {
                const KernelServerEntityState& state = query_buffer_[index];
                if (state.valid == 0u ||
                    carrier_for(state.entity_template_id) == nullptr) {
                    continue;
                }
                live_carriers.push_back(state.net_id);
                const auto existing = std::find_if(
                    instances_.begin(),
                    instances_.end(),
                    [&state](const Instance& instance) {
                        return instance.carrier_net_id == state.net_id;
                    });
                if (existing != instances_.end()) {
                    continue;
                }
                Instance instance;
                instance.carrier_net_id = state.net_id;
                instance.entity_template_id = state.entity_template_id;
                // Staggered by one interval rather than firing the tick it is
                // discovered, so a nest does not empty itself the instant it
                // appears.
                instance.ticks_until_spawn =
                    carrier_for(state.entity_template_id)->spawner.interval_ticks;
                instances_.push_back(std::move(instance));
            }
            break;
        }
    }

    // A carrier that is gone takes its rule with it. What it put out stays.
    instances_.erase(
        std::remove_if(
            instances_.begin(),
            instances_.end(),
            [this, &live_carriers](const Instance& instance) {
                // Only clocks are discovered by query; a caller's instance is
                // the alert pass's to retire.
                return trigger_of(instance) == SpawnerTrigger::kInterval &&
                    std::find(
                        live_carriers.begin(),
                        live_carriers.end(),
                        instance.carrier_net_id) == live_carriers.end();
            }),
        instances_.end());

    for (Instance& instance : instances_) {
        const SpawnerCarrierConfig* carrier =
            carrier_for(instance.entity_template_id);
        if (carrier == nullptr) {
            continue;
        }
        const SpawnerConfig& spawner = carrier->spawner;
        if (spawner.trigger != SpawnerTrigger::kInterval) {
            continue;
        }

        // Counted every tick, so a nest held at its ceiling for a while puts
        // out its next wave as soon as there is room rather than waiting out a
        // fresh interval on top.
        if (instance.ticks_until_spawn > 0) {
            --instance.ticks_until_spawn;
        }

        instance.spawned_net_ids.erase(
            std::remove_if(
                instance.spawned_net_ids.begin(),
                instance.spawned_net_ids.end(),
                [kernel](std::uint32_t net_id) {
                    return !entity_alive(kernel, net_id);
                }),
            instance.spawned_net_ids.end());

        if (instance.ticks_until_spawn > 0) {
            continue;
        }

        KernelServerEntityState carrier_state{};
        carrier_state.struct_size = sizeof(carrier_state);
        if (!Kernel_ServerGetEntityState(
                kernel, instance.carrier_net_id, &carrier_state) ||
            carrier_state.valid == 0u) {
            continue;
        }

        std::uint64_t random_state =
            static_cast<std::uint64_t>(spawner.seed) * 0x9e3779b97f4a7c15ull +
            static_cast<std::uint64_t>(instance.carrier_net_id) *
                0xbf58476d1ce4e5b9ull +
            static_cast<std::uint64_t>(instance.spawn_ordinal);
        std::uint32_t count =
            next_in_range(&random_state, spawner.count_min, spawner.count_max);
        if (spawner.max_live_agents != 0u) {
            const auto live =
                static_cast<std::uint32_t>(instance.spawned_net_ids.size());
            const std::uint32_t room =
                live >= spawner.max_live_agents ? 0u
                                                : spawner.max_live_agents - live;
            // Whole waves only. A wave cannot be trimmed to fit: the
            // composition's minimums are assigned before anything is drawn, so
            // asking for fewer units than the floors sum to returns the floors
            // anyway -- the trim would be silently ignored and the ceiling
            // exceeded. So the nest waits for room for a legal wave instead.
            count = std::min(count, room);
            if (count < spawner.count_min) {
                continue;
            }
        }

        if (emit_wave(kernel, &instance, *carrier, carrier_state, count,
                      &random_state) == 0u) {
            continue;
        }
        instance.ticks_until_spawn = spawner.interval_ticks;
    }
}

void SpawnerDirector::tick_alert_carriers(
    KernelHandle* kernel,
    const std::vector<AlertSignal>& alerts) {
    // The shared budget counts what callers put out, for as long as it lives
    // -- not for as long as its caller does. A caller dying must not hand its
    // share back, or killing callers one by one would let the next caller
    // stack another wave on top of the survivors.
    const auto prune = [kernel](std::vector<std::uint32_t>* net_ids) {
        net_ids->erase(
            std::remove_if(
                net_ids->begin(),
                net_ids->end(),
                [kernel](std::uint32_t net_id) {
                    return !entity_alive(kernel, net_id);
                }),
            net_ids->end());
    };
    prune(&reinforce_live_net_ids_);
    prune(&cannot_call_net_ids_);

    // Every alert instance counts down every tick, engaged or not: the gap
    // between calls carries across engagements, so a caller flickering in and
    // out of alert cannot call on every flicker.
    for (Instance& instance : instances_) {
        if (trigger_of(instance) != SpawnerTrigger::kOnAlert) {
            continue;
        }
        if (instance.ticks_until_spawn > 0) {
            --instance.ticks_until_spawn;
        }
    }
    // A caller that is gone takes its rule with it. What it put out stays, and
    // stays counted against the shared budget above.
    instances_.erase(
        std::remove_if(
            instances_.begin(),
            instances_.end(),
            [this, kernel](const Instance& instance) {
                return trigger_of(instance) == SpawnerTrigger::kOnAlert &&
                    !entity_alive(kernel, instance.carrier_net_id);
            }),
        instances_.end());

    if (alerts.empty()) {
        return;
    }
    // In net id order, so who is served first when the budget is short does
    // not depend on the order agents happen to sit in the manager's list.
    std::vector<const AlertSignal*> ordered;
    ordered.reserve(alerts.size());
    for (const AlertSignal& signal : alerts) {
        ordered.push_back(&signal);
    }
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const AlertSignal* left, const AlertSignal* right) {
            return left->net_id < right->net_id;
        });

    for (const AlertSignal* signal : ordered) {
        const SpawnerCarrierConfig* carrier =
            carrier_for(signal->entity_template_id);
        if (carrier == nullptr ||
            carrier->spawner.trigger != SpawnerTrigger::kOnAlert) {
            continue;
        }
        const SpawnerConfig& spawner = carrier->spawner;
        auto found = std::find_if(
            instances_.begin(),
            instances_.end(),
            [signal](const Instance& candidate) {
                return candidate.carrier_net_id == signal->net_id;
            });
        if (found == instances_.end()) {
            // First engagement: the rule starts here, and a first call is not
            // made to wait out an interval -- the moment of spotting someone
            // is the moment to call.
            Instance instance;
            instance.carrier_net_id = signal->net_id;
            instance.entity_template_id = signal->entity_template_id;
            instances_.push_back(std::move(instance));
            found = instances_.end() - 1;
        }
        Instance& instance = *found;
        if (signal->engagement_started) {
            instance.calls_this_engagement = 0;
        }

        // Plan B: only with the target in sight. An engagement outlives sight
        // by up to two forget windows, and calling for help at empty ground in
        // that tail reads as a bug.
        if (!signal->sees_target || instance.ticks_until_spawn > 0) {
            continue;
        }
        if (spawner.calls_per_alert != 0u &&
            instance.calls_this_engagement >= spawner.calls_per_alert) {
            continue;
        }
        if (std::find(
                cannot_call_net_ids_.begin(),
                cannot_call_net_ids_.end(),
                signal->net_id) != cannot_call_net_ids_.end()) {
            continue;
        }

        instance.spawned_net_ids.erase(
            std::remove_if(
                instance.spawned_net_ids.begin(),
                instance.spawned_net_ids.end(),
                [kernel](std::uint32_t net_id) {
                    return !entity_alive(kernel, net_id);
                }),
            instance.spawned_net_ids.end());

        std::uint64_t random_state =
            static_cast<std::uint64_t>(spawner.seed) * 0x9e3779b97f4a7c15ull +
            static_cast<std::uint64_t>(instance.carrier_net_id) *
                0xbf58476d1ce4e5b9ull +
            static_cast<std::uint64_t>(instance.spawn_ordinal);
        std::uint32_t count =
            next_in_range(&random_state, spawner.count_min, spawner.count_max);
        // Two ceilings, the caller's own and the shared one, and waves stay
        // whole under both: a wave cannot be trimmed, for the reason the
        // interval path gives. Refused for room is not a call made -- neither
        // the count nor the gap is spent, so the caller tries again on the next
        // tick it is still engaged and in sight, and gets in as soon as there is
        // room.
        const auto room_under = [](std::uint32_t ceiling, std::size_t live) {
            if (ceiling == 0u) {
                return UINT32_MAX;
            }
            return live >= ceiling
                ? 0u
                : ceiling - static_cast<std::uint32_t>(live);
        };
        count = std::min(
            count,
            room_under(spawner.max_live_agents, instance.spawned_net_ids.size()));
        count = std::min(
            count,
            room_under(reinforce_max_live_agents_, reinforce_live_net_ids_.size()));
        if (count < spawner.count_min) {
            continue;
        }

        KernelServerEntityState carrier_state{};
        carrier_state.struct_size = sizeof(carrier_state);
        if (!Kernel_ServerGetEntityState(
                kernel, instance.carrier_net_id, &carrier_state) ||
            carrier_state.valid == 0u) {
            continue;
        }
        if (emit_wave(kernel, &instance, *carrier, carrier_state, count,
                      &random_state) == 0u) {
            continue;
        }
        ++instance.calls_this_engagement;
        instance.ticks_until_spawn = spawner.interval_ticks;
    }
}

std::uint32_t SpawnerDirector::emit_wave(
    KernelHandle* kernel,
    Instance* instance,
    const SpawnerCarrierConfig& carrier,
    const KernelServerEntityState& carrier_state,
    std::uint32_t count,
    std::uint64_t* random_state) {
    const SpawnerConfig& spawner = carrier.spawner;
    const bool calls_for_help = spawner.trigger == SpawnerTrigger::kOnAlert;
    SpawnAreaConfig area;
    area.shape = SpawnAreaShape::kCircle;
    area.half_extents.x = spawner.radius;

    const std::vector<std::uint32_t> drawn =
        draw_spawn_composition(spawner.composition, count, random_state);
    const bool walks_out =
        spawner.entry.authored && !spawner.entry.exits.empty();
    std::uint32_t created = 0;
    for (std::size_t entry = 0; entry < drawn.size(); ++entry) {
        for (std::uint32_t unit = 0; unit < drawn[entry]; ++unit) {
            // Doors are dealt round robin across the whole wave, so a
            // carrier that names two of them puts half its units through
            // each rather than queueing everyone at the first.
            const SpawnerEntryExit* door = walks_out
                ? &spawner.entry.exits[created % spawner.entry.exits.size()]
                : nullptr;
            KernelServerEntityCreateInfo create_info{};
            create_info.struct_size = sizeof(create_info);
            create_info.owner_peer = 0;
            create_info.entity_template_id =
                spawner.composition[entry].entity_template_id;
            // Around wherever the carrier is now, not where it was placed:
            // a nest that has been knocked across the floor should emit
            // from where it ended up. A carrier with doors puts them at the
            // authored start instead, which is inside itself.
            create_info.position = door != nullptr
                ? carrier_world_point(carrier_state, door->start)
                : sample_area(
                      area,
                      carrier_world_point(carrier_state, spawner.offset),
                      random_state);
            // Facing the way the carrier faces, so a unit walking out is
            // already pointed at the door rather than turning on the spot.
            create_info.rotation =
                door != nullptr ? carrier_state.rotation : kIdentityRotation;
            std::uint32_t net_id = 0;
            if (!Kernel_ServerCreateEntity(kernel, &create_info, &net_id) ||
                net_id == 0) {
                spdlog::warn(
                    "spawner unit failed carrier={} template_id={}",
                    carrier.name,
                    spawner.composition[entry].entity_template_id);
                continue;
            }
            if (door != nullptr) {
                // Before the next Kernel_Update, so the unit's very first
                // physics tick already runs under the entry mask. Set any
                // later it would spend that tick blocked by the carrier it
                // is standing inside, and be shoved out through the wall.
                if (Kernel_ServerSetEntityMovementCollisionMask(
                        kernel,
                        net_id,
                        spawner.entry.movement_collision_mask)) {
                    SpawnerEntryRequest request;
                    request.net_id = net_id;
                    request.exit =
                        carrier_world_point(carrier_state, door->exit);
                    request.hold_ticks =
                        created * spawner.entry.stagger_ticks;
                    request.max_ticks = spawner.entry.max_ticks;
                    pending_entries_.push_back(request);
                } else {
                    // No request, so nothing will drive or release it: it is
                    // an ordinary unit that happens to be standing in the
                    // carrier, and the carrier pushes it out. Worth a line,
                    // because that is a visible difference.
                    spdlog::warn(
                        "spawner entry mask failed carrier={} net_id={}",
                        carrier.name,
                        net_id);
                }
            }
            instance->spawned_net_ids.push_back(net_id);
            if (calls_for_help) {
                reinforce_live_net_ids_.push_back(net_id);
            }
            // Anything a spawner put out may not call for help itself, or one
            // caller's wave of callers would chain. Kept only for units that
            // could call at all, so the list stays the size of the problem.
            if (is_alert_carrier(create_info.entity_template_id)) {
                cannot_call_net_ids_.push_back(net_id);
            }
            ++created;
        }
    }

    if (created == 0) {
        return 0;
    }
    ++instance->spawn_ordinal;
    spawned_unit_count_ += created;
    spdlog::info(
        "spawner emitted carrier={} net_id={} units={} live={}",
        carrier.name,
        instance->carrier_net_id,
        created,
        instance->spawned_net_ids.size());
    return created;
}

const std::vector<SpawnerCarrierConfig>& SpawnerDirector::carriers() const {
    return carriers_;
}

const std::vector<SpawnerDirector::Instance>& SpawnerDirector::instances() const {
    return instances_;
}

std::uint32_t SpawnerDirector::spawned_unit_count() const {
    return spawned_unit_count_;
}

std::uint32_t SpawnerDirector::reinforce_live_count() const {
    return static_cast<std::uint32_t>(reinforce_live_net_ids_.size());
}

std::vector<SpawnerEntryRequest> SpawnerDirector::take_pending_entries() {
    std::vector<SpawnerEntryRequest> taken = std::move(pending_entries_);
    pending_entries_.clear();
    return taken;
}

}  // namespace network_example::game_server
