#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "physics/public/physics_world.h"
#include "simulation/public/movement_solver.h"
#include "simulation/public/simulation.h"

namespace {

constexpr float kDeltaSeconds = 1.0f / 30.0f;
constexpr std::uint32_t kWarmupTicks = 120;
constexpr std::uint32_t kMeasuredTicks = 600;
constexpr std::uint32_t kRuns = 5;

enum class Variant { kDirect, kKinematic, kCharacter };

const char* variant_name(Variant variant) {
    switch (variant) {
        case Variant::kDirect:
            return "direct";
        case Variant::kKinematic:
            return "kinematic";
        case Variant::kCharacter:
            return "character";
    }
}

network_example::MovementState::ControllerType controller_for(Variant variant) {
    return variant == Variant::kCharacter
        ? network_example::MovementState::ControllerType::kCharacter
        : variant == Variant::kKinematic
            ? network_example::MovementState::ControllerType::kKinematic
            : network_example::MovementState::ControllerType::kNone;
}

struct ActorRecord {
    network_example::NetId net_id = 0;
    network_example::PeerId peer_id = 0;
    entt::entity entity = entt::null;
    std::uint32_t movement_collider_id = 0;
    std::uint32_t hitbox_collider_id = 0;
    glm::vec3 local_center{0.0f, 0.9f, 0.0f};
};

class Scenario {
public:
    Scenario(Variant variant, std::uint32_t actor_count)
        : variant_(variant), physics_({0, true}), world_(false) {
        world_.set_collision_world(&physics_);
        add_box(
            90000,
            {20.0f, -3.5f, static_cast<float>(actor_count) * 10.0f},
            {40.0f, 0.5f, static_cast<float>(actor_count) * 10.0f + 20.0f});
        actors_.reserve(actor_count);
        for (std::uint32_t index = 0; index < actor_count; ++index) {
            const float lane_z = static_cast<float>(index) * 20.0f;
            add_box(
                100000 + index * 3,
                {4.0f, -0.5f, lane_z},
                {8.0f, 0.5f, 8.0f});
            add_box(
                100001 + index * 3,
                {2.0f, 0.15f, lane_z},
                {0.25f, 0.15f, 1.0f});
            add_box(
                100002 + index * 3,
                {5.0f, 1.0f, lane_z},
                {0.25f, 1.0f, 1.0f});
            add_actor(index, lane_z);
        }
        sync_bodies(true);
    }

    void tick(
        const std::vector<network_example::QueuedInput>& inputs,
        std::uint32_t tick,
        network_example::MovementSimulationStats* stats) {
        if (variant_ == Variant::kDirect) {
            for (std::size_t index = 0; index < actors_.size(); ++index) {
                const ActorRecord& actor = actors_[index];
                const glm::vec3 velocity =
                    network_example::movement_solver::input_move_to_world(
                        inputs[index].input) * 5.0f;
                world_.registry().get<network_example::Velocity>(actor.entity)
                    .linear = velocity;
                world_.registry().get<network_example::Transform>(actor.entity)
                    .position += velocity * kDeltaSeconds;
            }
        } else {
            network_example::simulate_actor_movement(
                world_, inputs, kDeltaSeconds, tick, &events_, stats);
        }
        sync_bodies(false);
    }

    bool states_are_legal() const {
        for (const ActorRecord& actor : actors_) {
            const glm::vec3 position =
                world_.registry().get<network_example::Transform>(actor.entity)
                    .position;
            if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                !std::isfinite(position.z)) {
                return false;
            }
            if (variant_ != Variant::kDirect) {
                const auto state = world_.registry()
                    .get<network_example::MovementState>(actor.entity)
                    .ground_state;
                if (state != network_example::MovementState::GroundState::kAirborne &&
                    state != network_example::MovementState::GroundState::kGrounded &&
                    state != network_example::MovementState::GroundState::kSteepGround) {
                    return false;
                }
            }
        }
        return true;
    }

    float first_actor_x() const {
        return world_.registry()
            .get<network_example::Transform>(actors_.front().entity)
            .position.x;
    }

    network_example::physics::CollisionQueryStats query_stats() const {
        return physics_.query_stats();
    }

    void reset_query_stats() { physics_.reset_query_stats(); }

private:
    void add_box(
        std::uint32_t collider_id,
        const glm::vec3& position,
        const glm::vec3& half_extents) {
        network_example::physics::CollisionObjectDescriptor object{};
        object.identity = {
            0,
            collider_id,
            0,
            network_example::physics::CollisionObjectKind::kStaticObstacle,
            network_example::physics::CollisionLayer::kStaticObstacle,
        };
        object.shape.type =
            network_example::physics::CollisionShapeType::kBox;
        object.shape.half_extents = half_extents;
        object.position = position;
        std::string error;
        assert(physics_.upsert_object(object, &error));
    }

    void add_actor(std::uint32_t index, float lane_z) {
        const auto net_id = world_.spawn_player(
            index + 1, {0.0f, 0.0f, lane_z});
        const std::optional<entt::entity> found = world_.find_entity(net_id);
        assert(found.has_value());
        auto& movement = world_.registry()
            .get<network_example::MovementState>(*found);
        movement.speed_meters_per_second = 5.0f;
        movement.controller_type = controller_for(variant_);
        movement.movement_collider_template_id = 10;
        movement.gravity = {0.0f, -9.81f, 0.0f};
        movement.max_slope_degrees = 50.0f;
        movement.step_height = 0.4f;
        movement.ground_probe_distance = 0.25f;
        movement.ground_snap_distance = 0.5f;

        network_example::ColliderInstance collider{};
        collider.collider_template_id = 10;
        collider.owner_net_id = net_id;
        collider.entity_net_id = net_id;
        collider.entity_type = network_example::EntityType::kActor;
        collider.actor_type = network_example::ActorType::kPlayer;
        collider.shape_type = network_example::ColliderShapeType::kCapsule;
        collider.purpose_flags = KernelColliderPurpose_Movement;
        collider.local_center = {0.0f, 0.9f, 0.0f};
        collider.radius = 0.35f;
        collider.capsule_half_height = 0.55f;
        auto& stored = world_.collider_registry().upsert_entity_collider(
            net_id, 10, collider);
        movement.movement_collider_id = stored.collider_id;
        actors_.push_back(ActorRecord{
            net_id,
            index + 1,
            *found,
            stored.collider_id,
            0x40000000u + net_id,
        });
    }

    void sync_bodies(bool create) {
        for (const ActorRecord& actor : actors_) {
            const auto& transform = world_.registry()
                .get<network_example::Transform>(actor.entity);
            if (create) {
                network_example::physics::CollisionObjectDescriptor movement{};
                movement.identity = {
                    actor.net_id,
                    actor.movement_collider_id,
                    0,
                    network_example::physics::CollisionObjectKind::kActorMovement,
                    network_example::physics::CollisionLayer::kActorMovement,
                };
                movement.shape.type =
                    network_example::physics::CollisionShapeType::kCapsule;
                movement.shape.radius = 0.35f;
                movement.shape.capsule_half_height = 0.55f;
                movement.position = transform.position + actor.local_center;
                std::string error;
                assert(physics_.upsert_object(movement, &error));

                network_example::physics::CollisionObjectDescriptor hitbox{};
                hitbox.identity = {
                    actor.net_id,
                    actor.hitbox_collider_id,
                    1,
                    network_example::physics::CollisionObjectKind::kActorHitbox,
                    network_example::physics::CollisionLayer::kDamageable,
                    0,
                    network_example::physics::kGameplayCategoryPlayerSide,
                };
                hitbox.shape.type =
                    network_example::physics::CollisionShapeType::kBox;
                hitbox.shape.half_extents = {0.35f, 0.9f, 0.35f};
                hitbox.position = transform.position + actor.local_center;
                assert(physics_.upsert_object(hitbox, &error));
            } else {
                assert(physics_.set_object_transform(
                    actor.movement_collider_id,
                    transform.position + actor.local_center,
                    transform.rotation));
                assert(physics_.set_object_transform(
                    actor.hitbox_collider_id,
                    transform.position + actor.local_center,
                    transform.rotation));
            }
        }
    }

    Variant variant_;
    network_example::physics::PhysicsWorld physics_;
    network_example::World world_;
    std::vector<ActorRecord> actors_;
    std::vector<KernelEvent> events_;
};

std::vector<std::vector<network_example::QueuedInput>> make_inputs(
    std::uint32_t actor_count,
    std::uint32_t tick_count,
    bool idle) {
    std::vector<std::vector<network_example::QueuedInput>> all(tick_count);
    for (std::uint32_t tick = 0; tick < tick_count; ++tick) {
        glm::vec2 move{0.0f};
        if (!idle) {
            if (tick < 120) {
                move = tick < 60 ? glm::vec2{1.0f, 0.0f}
                                 : glm::vec2{1.0f, 1.0f};
            } else if (tick < 240) {
                move = {1.0f, 0.0f};
            } else if (tick < 360) {
                move = {1.0f, 1.0f};
            } else if (tick < 480) {
                move = {1.0f, 0.0f};
            } else {
                move = {0.0f, 1.0f};
            }
        }
        all[tick].reserve(actor_count);
        for (std::uint32_t actor = 0; actor < actor_count; ++actor) {
            PlayerInput input{};
            input.input_seq = tick + 1;
            input.move = KernelVec2{move.x, move.y};
            all[tick].push_back(network_example::QueuedInput{
                actor + 1,
                input,
                tick,
                0,
                false,
                actor + 1,
            });
        }
    }
    return all;
}

struct RunResult {
    std::vector<double> tick_us;
    network_example::MovementSimulationStats movement{};
    network_example::physics::CollisionQueryStats queries{};
    bool step_condition = false;
};

RunResult run_once(Variant variant, std::uint32_t actor_count) {
    Scenario scenario(variant, actor_count);
    const auto warmup_inputs = make_inputs(actor_count, kWarmupTicks, true);
    const auto measured_inputs = make_inputs(actor_count, kMeasuredTicks, false);
    network_example::MovementSimulationStats discarded{};
    for (std::uint32_t tick = 0; tick < kWarmupTicks; ++tick) {
        scenario.tick(warmup_inputs[tick], tick, &discarded);
    }
    scenario.reset_query_stats();

    RunResult result{};
    result.tick_us.reserve(kMeasuredTicks);
    for (std::uint32_t tick = 0; tick < kMeasuredTicks; ++tick) {
        const auto start = std::chrono::steady_clock::now();
        scenario.tick(
            measured_inputs[tick], kWarmupTicks + tick, &result.movement);
        const auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        assert(std::isfinite(elapsed));
        result.tick_us.push_back(elapsed);
        assert(scenario.states_are_legal());
        if (tick == 59 && variant != Variant::kDirect) {
            result.step_condition = variant == Variant::kCharacter
                ? scenario.first_actor_x() > 3.0f
                : scenario.first_actor_x() < 1.5f;
        }
    }
    result.queries = scenario.query_stats();
    assert(result.tick_us.size() == kMeasuredTicks);
    if (variant != Variant::kDirect) {
        assert(result.step_condition);
    }
    return result;
}

double percentile(std::vector<double> values, double fraction) {
    assert(!values.empty());
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        fraction * static_cast<double>(values.size() - 1));
    return values[index];
}

struct Aggregate {
    std::vector<double> samples;
    std::uint64_t grounding_calls = 0;
    std::uint64_t shape_cast_calls = 0;
    std::uint64_t character_calls = 0;
};

void append(Aggregate* aggregate, const RunResult& run) {
    aggregate->samples.insert(
        aggregate->samples.end(), run.tick_us.begin(), run.tick_us.end());
    aggregate->grounding_calls += run.movement.grounded_query_count;
    aggregate->shape_cast_calls += run.queries.shape_cast_query_count;
    aggregate->character_calls += run.movement.character_move_count;
}

double average(const Aggregate& aggregate) {
    return std::accumulate(
        aggregate.samples.begin(), aggregate.samples.end(), 0.0) /
        static_cast<double>(aggregate.samples.size());
}

void print_result(
    Variant variant,
    std::uint32_t actor_count,
    const Aggregate& aggregate) {
    const double avg = average(aggregate);
    const double per_actor = avg / static_cast<double>(actor_count);
    std::cout << variant_name(variant)
              << ",actors=" << actor_count
              << ",avg_us=" << avg
              << ",p50_us=" << percentile(aggregate.samples, 0.50)
              << ",p95_us=" << percentile(aggregate.samples, 0.95)
              << ",us_per_player_tick=" << per_actor
              << ",budget_percent=" << avg / 33333.333333 * 100.0
              << ",grounding_calls=" << aggregate.grounding_calls
              << ",shape_cast_calls=" << aggregate.shape_cast_calls
              << ",character_calls=" << aggregate.character_calls
              << ",samples=" << aggregate.samples.size()
              << '\n';
}

}  // namespace

int main() {
    std::cout << std::fixed << std::setprecision(3);
    const std::vector<std::uint32_t> actor_counts{1, 16, 64, 256};
    const std::vector<Variant> interleaved{
        Variant::kKinematic,
        Variant::kCharacter,
        Variant::kCharacter,
        Variant::kKinematic,
        Variant::kKinematic,
        Variant::kCharacter,
        Variant::kCharacter,
        Variant::kKinematic,
        Variant::kKinematic,
        Variant::kCharacter,
    };
    for (std::uint32_t actor_count : actor_counts) {
        Aggregate direct{};
        Aggregate kinematic{};
        Aggregate character{};
        for (std::uint32_t run = 0; run < kRuns; ++run) {
            append(&direct, run_once(Variant::kDirect, actor_count));
        }
        for (Variant variant : interleaved) {
            append(
                variant == Variant::kKinematic ? &kinematic : &character,
                run_once(variant, actor_count));
        }
        assert(direct.samples.size() == kRuns * kMeasuredTicks);
        assert(kinematic.samples.size() == kRuns * kMeasuredTicks);
        assert(character.samples.size() == kRuns * kMeasuredTicks);
        print_result(Variant::kDirect, actor_count, direct);
        print_result(Variant::kKinematic, actor_count, kinematic);
        print_result(Variant::kCharacter, actor_count, character);
        const double kinematic_avg = average(kinematic);
        const double character_avg = average(character);
        assert(std::isfinite(kinematic_avg));
        assert(std::isfinite(character_avg));
        std::cout << "player_ab,actors=" << actor_count
                  << ",kinematic_avg_us=" << kinematic_avg
                  << ",character_avg_us=" << character_avg
                  << ",character_to_kinematic_ratio="
                  << character_avg / kinematic_avg
                  << '\n';
    }
    return 0;
}
