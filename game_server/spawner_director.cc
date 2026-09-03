#include "game_server/spawner_director.h"

#include <algorithm>
#include <utility>

#include <spdlog/spdlog.h>

#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

constexpr KernelQuat kIdentityRotation{0.0f, 0.0f, 0.0f, 1.0f};
constexpr std::size_t kInitialQueriedEntities = 64;
constexpr std::size_t kMaxQueriedEntities = 8192;

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
    return {};
}

SpawnerDirector::SpawnerDirector(std::vector<SpawnerCarrierConfig> carriers)
    : carriers_(std::move(carriers)) {
    for (const SpawnerCarrierConfig& carrier : carriers_) {
        if (std::find(
                queried_entity_types_.begin(),
                queried_entity_types_.end(),
                carrier.entity_type) == queried_entity_types_.end()) {
            queried_entity_types_.push_back(carrier.entity_type);
        }
    }
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

void SpawnerDirector::tick(KernelHandle* kernel) {
    if (kernel == nullptr || carriers_.empty()) {
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
            [&live_carriers](const Instance& instance) {
                return std::find(
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

        SpawnAreaConfig area;
        area.shape = SpawnAreaShape::kCircle;
        area.half_extents.x = spawner.radius;

        const std::vector<std::uint32_t> drawn =
            draw_spawn_composition(spawner.composition, count, &random_state);
        std::uint32_t created = 0;
        for (std::size_t entry = 0; entry < drawn.size(); ++entry) {
            for (std::uint32_t unit = 0; unit < drawn[entry]; ++unit) {
                KernelServerEntityCreateInfo create_info{};
                create_info.struct_size = sizeof(create_info);
                create_info.owner_peer = 0;
                create_info.entity_template_id =
                    spawner.composition[entry].entity_template_id;
                // Around wherever the carrier is now, not where it was placed:
                // a nest that has been knocked across the floor should emit
                // from where it ended up.
                create_info.position =
                    sample_area(area, carrier_state.position, &random_state);
                create_info.rotation = kIdentityRotation;
                std::uint32_t net_id = 0;
                if (!Kernel_ServerCreateEntity(kernel, &create_info, &net_id) ||
                    net_id == 0) {
                    spdlog::warn(
                        "spawner unit failed carrier={} template_id={}",
                        carrier->name,
                        spawner.composition[entry].entity_template_id);
                    continue;
                }
                instance.spawned_net_ids.push_back(net_id);
                ++created;
            }
        }

        if (created == 0) {
            continue;
        }
        ++instance.spawn_ordinal;
        spawned_unit_count_ += created;
        instance.ticks_until_spawn = spawner.interval_ticks;
        spdlog::info(
            "spawner emitted carrier={} net_id={} units={} live={}",
            carrier->name,
            instance.carrier_net_id,
            created,
            instance.spawned_net_ids.size());
    }
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

}  // namespace network_example::game_server
