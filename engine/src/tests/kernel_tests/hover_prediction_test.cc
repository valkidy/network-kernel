// P4 of docs/PLAYER_HOVER_PLAN.md: the owner's prediction of its own status
// hover. The authority's movement step and the client's prediction step run
// side by side here, on the same ground with the same inputs, and must put the
// player in the same place every tick -- the gap the reconciliation would
// otherwise snap back. Then the order (§2.1) on the prediction alone: the
// dead, the sheltered, a suspension and the drop after one come first.
//
// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/public/kernel_api.h"
#include "physics/public/physics_world.h"
#include "simulation/public/movement_solver.h"
#include "world/public/components.h"

#define private public
#include "kernel/src/kernel.h"
#undef private
#include "simulation/public/simulation.h"

namespace ne = network_example;

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "hover_prediction_test:%d: %s\n", line, expression);
    std::abort();
}
#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr float kTick = 1.0f / 30.0f;
constexpr float kHeight = 3.0f;
constexpr float kClimb = 4.0f;

struct Box {
    glm::vec3 center;
    glm::vec3 half;
};

// The ground, and a 1 m box to ride over.
const std::vector<Box> kScene{
    {glm::vec3{0.0f, -0.5f, 0.0f}, glm::vec3{40.0f, 0.5f, 40.0f}},
    {glm::vec3{6.0f, 0.5f, 0.0f}, glm::vec3{1.5f, 0.5f, 3.0f}},
};

void install(ne::physics::PhysicsWorld* physics, std::uint32_t first_id) {
    std::uint32_t id = first_id;
    for (const Box& box : kScene) {
        ne::physics::CollisionObjectDescriptor object{};
        object.identity.collider_id = id++;
        object.identity.kind = ne::physics::CollisionObjectKind::kTerrain;
        object.identity.layer = ne::physics::CollisionLayer::kTerrain;
        object.shape.type = ne::physics::CollisionShapeType::kBox;
        object.shape.half_extents = box.half;
        object.position = box.center;
        std::string error;
        require(physics->upsert_object(object, &error));
    }
}

// client_mode_test's prepare_character_prediction: a client whose local player
// is a character on its own prediction world.
void prepare_client(ne::KernelEngine* engine) {
    engine->reset_runtime_state(KernelMode_Client);
    engine->local_player_net_id_ = 1;
    engine->has_predicted_local_entity_ = true;
    engine->has_authoritative_local_entity_ = true;
    engine->predicted_local_entity_.net_id = 1;
    engine->predicted_local_entity_.type = ne::EntityType::kActor;
    engine->predicted_local_entity_.actor_type = ne::ActorType::kPlayer;
    engine->predicted_local_entity_.has_authoritative_movement_state = true;
    engine->predicted_character_state_.position = glm::vec3{0.0f};
    engine->predicted_character_state_.ground_state =
        ne::physics::CharacterGroundState::kGrounded;

    engine->client_replicated_entities_.push_back({});
    auto& replicated = engine->client_replicated_entities_.back();
    replicated.net_id = 1;
    replicated.type = ne::EntityType::kActor;
    replicated.actor_type = ne::ActorType::kPlayer;
    replicated.actor_template_id = 1;
    replicated.collider_template_id = 1;
    replicated.active = true;

    KernelColliderTemplateDefinition capsule{};
    capsule.struct_size = sizeof(capsule);
    capsule.template_id = 11;
    capsule.shape_type = KernelColliderShapeType_Capsule;
    capsule.center = KernelVec3{0.0f, 0.9f, 0.0f};
    capsule.shape_params = KernelVec4{0.35f, 0.55f, 0.0f, 0.0f};
    capsule.layer_mask = KERNEL_COLLISION_LAYER_PLAYER_SIDE;
    capsule.purpose_flags = KernelColliderPurpose_Movement;
    engine->collider_templates_.push_back(capsule);

    KernelEntityTemplateDefinition player{};
    player.struct_size = sizeof(player);
    player.entity_template_id = 101;
    player.actor_type = KernelActorType_Player;
    player.actor_template_id = 1;
    player.combat.struct_size = sizeof(player.combat);
    player.combat.move_speed_meters_per_second = 5.0f;
    player.movement.struct_size = sizeof(player.movement);
    player.movement.controller_type = KernelMovementControllerType_Character;
    player.movement.movement_collider_template_id = 11;
    player.movement.gravity = KernelVec3{0.0f, -9.81f, 0.0f};
    player.movement.max_slope_degrees = 50.0f;
    player.movement.step_height = 0.4f;
    player.movement.ground_probe_distance = 0.25f;
    player.movement.ground_snap_distance = 0.5f;
    engine->entity_templates_.push_back(player);
    engine->local_player_move_speed_meters_per_second_ = 5.0f;

    engine->prediction_physics_world_ =
        std::make_unique<ne::physics::PhysicsWorld>(ne::physics::PhysicsWorldConfig{});
    require(engine->prediction_physics_world_->valid());
    install(engine->prediction_physics_world_.get(), 900u);
}

// The authority: the same character, by the client's own movement config, on
// its own world.
struct Authority {
    explicit Authority(const ne::movement_solver::CharacterMovementConfig& config)
        : world(false) {
        install(&physics, 900u);
        world.set_collision_world(&physics);
        actor = world.spawn_player(7, glm::vec3{0.0f});
        entity = *world.find_entity(actor);
        ne::MovementState& movement = world.registry().get<ne::MovementState>(entity);
        movement.speed_meters_per_second = 5.0f;
        movement.controller_type = ne::MovementState::ControllerType::kCharacter;
        movement.movement_collider_template_id = 11;
        movement.gravity = config.gravity;
        movement.max_slope_degrees = config.max_slope_degrees;
        movement.step_height = config.step_height;
        movement.ground_probe_distance = 0.25f;
        movement.ground_snap_distance = config.ground_snap_distance;
        movement.movement_collision_mask = config.filter.collision_mask;

        ne::ColliderInstance collider{};
        collider.collider_template_id = 11;
        collider.owner_net_id = actor;
        collider.entity_net_id = actor;
        collider.entity_type = ne::EntityType::kActor;
        collider.actor_type = ne::ActorType::kPlayer;
        collider.shape_type = ne::ColliderShapeType::kCapsule;
        collider.purpose_flags = KernelColliderPurpose_Movement;
        collider.local_center = config.shape.local_center;
        collider.world_center = config.shape.local_center;
        collider.radius = config.shape.radius;
        collider.capsule_half_height = config.shape.capsule_half_height;
        const ne::ColliderInstance& stored =
            world.collider_registry().upsert_entity_collider(actor, 11, collider);
        movement.movement_collider_id = stored.collider_id;
    }

    void tick(std::uint32_t tick_index, const KernelPlayerInput& input) {
        const std::vector<ne::QueuedInput> inputs{
            ne::QueuedInput{7, input, tick_index, 0, false, 0},
        };
        ne::simulate_actor_movement(world, inputs, kTick, tick_index, &events, &stats);
    }

    void hover(std::uint32_t until) {
        ne::ActiveStatusEffect active;
        active.instance_id = 1u;
        active.status_effect_id = 1006u;
        active.expire_tick = until;
        active.hovers = true;
        active.hover_height_meters = kHeight;
        active.hover_vertical_speed = kClimb;
        world.registry().get_or_emplace<ne::StatusEffectState>(entity).active.push_back(active);
    }

    // What simulate_status_effects does ahead of movement on the expiry tick.
    void expire(std::uint32_t tick_index) {
        auto& active = world.registry().get_or_emplace<ne::StatusEffectState>(entity).active;
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&](const ne::ActiveStatusEffect& a) {
                                        return a.expire_tick <= tick_index;
                                    }),
                     active.end());
    }

    glm::vec3 position() const { return world.registry().get<ne::Transform>(entity).position; }

    ne::physics::PhysicsWorld physics;
    ne::World world;
    ne::NetId actor = 0;
    entt::entity entity = entt::null;
    std::vector<KernelEvent> events;
    ne::MovementSimulationStats stats{};
};

KernelConfig client_config() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    return config;
}

ne::EntitySnapshot own_hover(std::uint32_t until) {
    ne::EntitySnapshot own;
    own.has_hover = true;
    own.hover_height_meters = kHeight;
    own.hover_vertical_speed = kClimb;
    own.hover_until_tick = until;
    return own;
}

KernelPlayerInput stick(std::uint32_t tick) {
    KernelPlayerInput input{};
    const float angle = 0.03f * static_cast<float>(tick);
    input.move.x = tick < 150u ? 1.0f : std::cos(angle);
    input.move.y = tick < 150u ? 0.0f : std::sin(angle);
    return input;
}

// Walk, drink at tick 20, hover across the box and around, the status ends at
// tick 260, and the fall after it: the prediction must be where the authority
// is, tick for tick.
void the_prediction_is_where_the_authority_is() {
    ne::KernelEngine client(client_config());
    prepare_client(&client);
    ne::movement_solver::CharacterMovementConfig config{};
    require(client.build_local_character_movement_config(&config));
    Authority authority(config);

    constexpr std::uint32_t kDrink = 20u;
    constexpr std::uint32_t kUntil = 260u;
    float worst = 0.0f;
    std::uint32_t worst_tick = 0u;
    float highest = 0.0f;
    int hovering_ticks = 0;
    for (std::uint32_t tick = 1u; tick < 330u; ++tick) {
        if (tick == kDrink) {
            authority.hover(kUntil);
            // The owner snapshot that says so; adopted on the tick it applies
            // from, the best case. The lag before it is the reconciliation's.
            client.adopt_authoritative_hover(own_hover(kUntil));
        }
        authority.expire(tick);
        const KernelPlayerInput input = stick(tick);
        authority.tick(tick, input);
        require(client.step_local_character_prediction(input, tick));
        const float gap =
            glm::length(authority.position() - client.predicted_character_state_.position);
        if (gap > worst) {
            worst = gap;
            worst_tick = tick;
        }
        highest = std::max(highest, authority.position().y);
        hovering_ticks += client.predicted_hovering_ ? 1 : 0;
        require(client.predicted_hovering_ ==
                authority.world.registry().get<ne::MovementState>(authority.entity).hovering);
    }
    std::printf("[parity] worst gap %.6f m at tick %u; highest %.3f m; %d ticks hovering\n",
                worst, worst_tick, highest, hovering_ticks);
    // It did hover -- over the box, at 1 + 3 m -- and came back down.
    require(highest > kHeight + 0.5f);
    require(hovering_ticks == static_cast<int>(kUntil - kDrink));
    require(authority.position().y < 0.05f);
    require(worst < 0.001f);
}

// The control for the gap above: a prediction that does not know about the
// hover is metres off.
void without_the_snapshot_it_would_be_snapped_back() {
    ne::KernelEngine client(client_config());
    prepare_client(&client);
    ne::movement_solver::CharacterMovementConfig config{};
    require(client.build_local_character_movement_config(&config));
    Authority authority(config);
    float worst = 0.0f;
    for (std::uint32_t tick = 1u; tick < 120u; ++tick) {
        if (tick == 20u) authority.hover(1000u);
        authority.tick(tick, stick(tick));
        require(client.step_local_character_prediction(stick(tick), tick));
        worst = std::max(worst, glm::length(
            authority.position() - client.predicted_character_state_.position));
    }
    std::printf("[control] worst gap with no hover predicted %.3f m\n", worst);
    require(worst > 2.0f);
}

// Hovering at height, on the prediction alone.
ne::KernelEngine* hovering_client(ne::KernelEngine* client, std::uint32_t* tick) {
    prepare_client(client);
    client->adopt_authoritative_hover(own_hover(100000u));
    for (*tick = 1u; *tick < 40u; ++*tick) {
        require(client->step_local_character_prediction(KernelPlayerInput{}, *tick));
    }
    require(std::abs(client->predicted_character_state_.position.y - kHeight) < 0.05f);
    require(client->predicted_hovering_);
    return client;
}

void what_comes_first_comes_first() {
    // Dead: the snapshot's DEAD flag lets it fall.
    {
        ne::KernelEngine client(client_config());
        std::uint32_t tick = 0u;
        hovering_client(&client, &tick);
        ne::EntitySnapshot dead = own_hover(100000u);
        dead.flags = ne::kVisualFlagDead;
        client.adopt_authoritative_hover(dead);
        for (int i = 0; i < 40; ++i, ++tick) {
            require(client.step_local_character_prediction(KernelPlayerInput{}, tick));
        }
        require(!client.predicted_hovering_);
        require(client.predicted_character_state_.position.y < 0.05f);
    }
    // Sheltered: no hover while inside.
    {
        ne::KernelEngine client(client_config());
        std::uint32_t tick = 0u;
        hovering_client(&client, &tick);
        client.predicted_shelter_net_id_ = 50u;
        require(client.step_local_character_prediction(KernelPlayerInput{}, tick));
        require(!client.predicted_hovering_);
    }
    // A bubble: the suspension moves it, then the drop it arms takes it to the
    // ground, and only after that does the hover lift it again.
    {
        ne::KernelEngine client(client_config());
        std::uint32_t tick = 0u;
        hovering_client(&client, &tick);
        ne::EntitySnapshot bubble;
        bubble.has_suspension = true;
        bubble.suspension_velocity = glm::vec3{0.0f, 1.0f, 0.0f};
        bubble.suspension_until_tick = tick + 30u;
        client.adopt_authoritative_suspension(bubble);
        for (int i = 0; i < 30; ++i, ++tick) {
            require(client.step_local_character_prediction(KernelPlayerInput{}, tick));
            require(!client.predicted_hovering_);
        }
        require(std::abs(client.predicted_character_state_.position.y - (kHeight + 1.0f)) < 0.05f);
        int landed_after = -1;
        for (int i = 0; i < 60 && landed_after < 0; ++i, ++tick) {
            require(client.step_local_character_prediction(KernelPlayerInput{}, tick));
            if (client.predicted_character_state_.ground_state ==
                ne::physics::CharacterGroundState::kGrounded) {
                landed_after = i;
            }
            require(!client.predicted_hovering_);
        }
        require(landed_after > 0);
        for (int i = 0; i < 40; ++i, ++tick) {
            require(client.step_local_character_prediction(KernelPlayerInput{}, tick));
        }
        std::printf("[bubble] prediction landed after %d ticks, back at %.4f\n",
                    landed_after, client.predicted_character_state_.position.y);
        require(client.predicted_hovering_);
        require(std::abs(client.predicted_character_state_.position.y - kHeight) < 0.05f);
    }
    // A snapshot without one clears it.
    {
        ne::KernelEngine client(client_config());
        std::uint32_t tick = 0u;
        hovering_client(&client, &tick);
        client.adopt_authoritative_hover(ne::EntitySnapshot{});
        require(client.predicted_hover_until_tick_ == 0u);
        require(client.step_local_character_prediction(KernelPlayerInput{}, tick));
        require(!client.predicted_hovering_);
    }
}

// The drawn body says HOVERING, not FALLING, while the prediction hovers it.
void the_drawn_body_says_hovering() {
    ne::KernelEngine client(client_config());
    std::uint32_t tick = 0u;
    hovering_client(&client, &tick);
    client.predicted_local_entity_.flags = ne::kVisualFlagFalling;
    client.render_states_.clear();
    client.append_predicted_local_render_state();
    require(!client.render_states_.empty());
    const std::uint32_t flags = client.render_states_.back().visual_flags;
    require((flags & ne::kVisualFlagHovering) != 0u);
    require((flags & (ne::kVisualFlagFalling | ne::kVisualFlagGrounded)) == 0u);
}

}  // namespace

int main() {
    the_prediction_is_where_the_authority_is();
    without_the_snapshot_it_would_be_snapped_back();
    what_comes_first_comes_first();
    the_drawn_body_says_hovering();
    std::printf("hover_prediction_test: PASS\n");
    return 0;
}
