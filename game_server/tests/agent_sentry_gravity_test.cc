// A sentry has to accumulate gravity.
//
// AgentSentryController enqueues a velocity for its agent every single tick,
// unconditionally. It used to build that velocity by zeroing all three axes at
// the top of the loop, and only the passive_patrol branch rebuilt it with the
// vertical component carried through. Every other sentry therefore told the
// kernel "my vertical speed is zero" thirty times a second: each tick of
// gravity was overwritten by the next enqueue, so vertical velocity never grew
// past a single tick's worth and an agent spawned above the ground effectively
// hung there.
//
// This is a separate target rather than an addition to
// agent_sentry_controller_test.cc on purpose: that file is a flat main() of
// bare asserts that currently aborts partway through on an unrelated
// perception assertion, so anything appended to it would never execute.
#include "game_server/src/agent_sentry_controller.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "game_server/src/ai_perception_adapter.h"
#include "game_server/src/gameplay_config.h"

namespace {

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "agent_sentry_gravity_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint32_t kAgentTemplateId = 1u;
constexpr std::uint32_t kAgentEntityTemplateId = 101u;
constexpr std::uint32_t kHitColliderId = 1u;
constexpr std::uint32_t kMovementColliderId = 11u;
constexpr float kSpawnHeight = 20.0f;
constexpr float kGravityY = -9.81f;
constexpr float kTickSeconds = 1.0f / 30.0f;

void load_catalog(KernelHandle* kernel) {
    KernelColliderTemplateDefinition hit{};
    hit.struct_size = sizeof(hit);
    hit.template_id = kHitColliderId;
    hit.shape_type = KernelColliderShapeType_Aabb;
    hit.center = KernelVec3{0.0f, 0.9f, 0.0f};
    hit.shape_params = KernelVec4{0.35f, 0.9f, 0.35f, 0.0f};
    hit.purpose_flags = KernelColliderPurpose_Hit;
    hit.layer_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;

    KernelColliderTemplateDefinition movement{};
    movement.struct_size = sizeof(movement);
    movement.template_id = kMovementColliderId;
    movement.shape_type = KernelColliderShapeType_Capsule;
    movement.center = KernelVec3{0.0f, 0.9f, 0.0f};
    movement.shape_params = KernelVec4{0.55f, 0.35f, 0.0f, 0.0f};
    movement.purpose_flags = KernelColliderPurpose_Movement;
    movement.layer_mask =
        KERNEL_COLLISION_LAYER_PLAYER_SIDE | KERNEL_COLLISION_LAYER_HOSTILE_SIDE;

    const std::array<KernelColliderTemplateDefinition, 2> collider_templates = {
        hit, movement};

    KernelActorTemplateDefinition agent_actor{};
    agent_actor.struct_size = sizeof(agent_actor);
    agent_actor.actor_template_id = kAgentTemplateId;
    agent_actor.entity_type = KernelEntityType_Actor;
    agent_actor.actor_type = KernelActorType_Agent;
    agent_actor.collider_template_id = kHitColliderId;
    agent_actor.vision.struct_size = sizeof(KernelAgentVisionConfig);
    agent_actor.vision.camp = KernelAgentCamp_EnemySide;

    KernelEntityTemplateDefinition agent_entity{};
    agent_entity.struct_size = sizeof(agent_entity);
    agent_entity.entity_template_id = kAgentEntityTemplateId;
    agent_entity.entity_type = KernelEntityType_Actor;
    agent_entity.actor_type = KernelActorType_Agent;
    agent_entity.actor_template_id = kAgentTemplateId;
    agent_entity.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM |
        KERNEL_ENTITY_COMPONENT_VELOCITY |
        KERNEL_ENTITY_COMPONENT_HEALTH |
        KERNEL_ENTITY_COMPONENT_HITBOX;
    agent_entity.collider_template_id = kHitColliderId;
    agent_entity.combat.struct_size = sizeof(agent_entity.combat);
    agent_entity.combat.hp = 100;
    agent_entity.combat.max_hp = 100;
    agent_entity.combat.move_speed_meters_per_second = 0.0f;
    agent_entity.combat.hitbox_center = KernelVec3{0.0f, 0.9f, 0.0f};
    agent_entity.combat.hitbox_half_extents = KernelVec3{0.35f, 0.9f, 0.35f};
    agent_entity.movement.struct_size = sizeof(KernelMovementDefinition);
    agent_entity.movement.controller_type =
        KernelMovementControllerType_Character;
    agent_entity.movement.movement_collider_template_id = kMovementColliderId;
    agent_entity.movement.gravity = KernelVec3{0.0f, kGravityY, 0.0f};
    agent_entity.movement.max_slope_degrees = 45.0f;
    agent_entity.movement.step_height = 0.4f;
    agent_entity.movement.ground_probe_distance = 0.25f;
    agent_entity.movement.ground_snap_distance = 0.5f;
    agent_entity.ai.struct_size = sizeof(KernelEntityAiDefinition);
    agent_entity.vision.struct_size = sizeof(KernelAgentVisionConfig);
    agent_entity.vision.camp = KernelAgentCamp_EnemySide;

    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 1;
    catalog.collider_templates = collider_templates.data();
    catalog.collider_template_count =
        static_cast<std::uint32_t>(collider_templates.size());
    catalog.actor_templates = &agent_actor;
    catalog.actor_template_count = 1;
    catalog.entity_templates = &agent_entity;
    catalog.entity_template_count = 1;
    require(Kernel_LoadGameplayCatalog(kernel, &catalog, nullptr));
}

// A ground plane at y = 0. The agent under test never reaches it -- see the
// note on a_sentry_spawned_in_the_air_falls -- but a server physics world
// without a static scene is not a configuration the game ever runs in.
std::vector<std::uint8_t> read_ground_scene() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    const std::filesystem::path path = std::filesystem::path(test_srcdir) /
        test_workspace / "game_server" / "shipping_catalog" / "mesh_assets" /
        "jolt" /
        "plane_200x200.joltmesh";
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

// One sentry, spawned in the air, driven exactly as AgentRuntimeManager drives
// it. passive_patrol is left at its default false -- the path the bug lived on.
struct SentryFixture {
    explicit SentryFixture(std::uint16_t port) {
        KernelConfig config{};
        config.mode = KernelMode_DedicatedServer;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 30;
        config.max_events = 64;
        config.max_render_states = 64;
        kernel = Kernel_Create(&config);
        require(kernel != nullptr);

        const std::vector<std::uint8_t> scene = read_ground_scene();
        KernelStaticCollisionSceneConfig scene_config{};
        scene_config.struct_size = sizeof(scene_config);
        scene_config.artifact_bytes = scene.data();
        scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
        scene_config.scene_id = 1u;
        scene_config.collider_id = 1u;
        scene_config.collision_layer = KERNEL_STATIC_COLLISION_LAYER_TERRAIN;
        require(Kernel_SetStaticCollisionScene(kernel, &scene_config));

        require(Kernel_StartDedicatedServer(kernel, port));
        load_catalog(kernel);

        KernelServerEntityCreateInfo create_info{};
        create_info.struct_size = sizeof(create_info);
        create_info.entity_type = network_example::game_server::kEntityTypeActor;
        create_info.actor_type = network_example::game_server::kActorTypeAgent;
        create_info.entity_template_id = kAgentEntityTemplateId;
        create_info.position = KernelVec3{0.0f, kSpawnHeight, 0.0f};
        create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
        require(net_id != 0);
        require(
            Kernel_ServerSetEntityActorTemplate(kernel, net_id, kAgentTemplateId));

        network_example::game_server::AgentRuntimeState agent;
        agent.net_id = net_id;
        agent.actor_template_id = kAgentTemplateId;
        agent.position = create_info.position;
        agent.sentry_config = network_example::game_server::AgentSentryConfig{};
        require(!agent.sentry_config.passive_patrol);
        agents.push_back(agent);
    }

    ~SentryFixture() {
        Kernel_Destroy(kernel);
    }

    void tick(std::uint32_t count = 1u) {
        for (std::uint32_t index = 0; index < count; ++index) {
            controller.tick(kernel, &agents, kTickSeconds);
            Kernel_Update(kernel, kTickSeconds);
        }
    }

    KernelServerEntityState state() const {
        KernelServerEntityState out{};
        out.struct_size = sizeof(out);
        require(Kernel_ServerGetEntityState(kernel, net_id, &out));
        require(out.valid != 0u);
        return out;
    }

    KernelHandle* kernel = nullptr;
    std::uint32_t net_id = 0;
    std::vector<network_example::game_server::AgentRuntimeState> agents;
    network_example::game_server::AgentSentryController controller;
};

// The tightest statement of the fix. The controller enqueues a velocity every
// tick and the kernel applies it before the movement step, so if the enqueued
// vertical component is zero the agent can never hold more than a single
// tick's worth of gravity -- a flat -0.327 m/s forever, whatever the drop.
void the_controller_no_longer_overwrites_vertical_velocity() {
    SentryFixture fixture(7811);
    constexpr std::uint32_t kTicks = 10;
    fixture.tick(kTicks);

    const float one_tick_of_gravity = kGravityY * kTickSeconds;
    const float free_fall = kGravityY * kTickSeconds * static_cast<float>(kTicks);
    const float measured = fixture.state().velocity.y;

    // Accumulated, not reset: comfortably past what one tick alone can produce.
    require(measured < one_tick_of_gravity * 2.0f);
    // And it is free fall, to within a tick of integration slack.
    require(std::fabs(measured - free_fall) < std::fabs(one_tick_of_gravity) * 1.5f);
}

// The positional consequence: the agent actually descends, and keeps
// descending. With the vertical velocity clobbered it crept at a fixed
// 0.011 m per tick instead, which reads as hanging in the air.
void a_sentry_spawned_in_the_air_falls() {
    SentryFixture fixture(7812);
    const float start = fixture.state().position.y;
    require(std::fabs(start - kSpawnHeight) < 0.001f);

    fixture.tick(10);
    const float early = fixture.state().position.y;
    fixture.tick(10);
    const float later = fixture.state().position.y;

    require(early < start);
    require(later < early);
    // The second ten ticks cover more ground than the first: the fall is
    // accelerating, which is exactly what the overwritten velocity prevented.
    require((early - later) > (start - early));
}

// The patrol branch always carried the vertical component through, and it is
// untouched by this fix. Pinning the two paths against each other is what
// keeps them from drifting apart again.
KernelVec3 fall_after(std::uint16_t port, bool passive_patrol, std::uint32_t ticks) {
    SentryFixture fixture(port);
    fixture.agents[0].sentry_config.passive_patrol = passive_patrol;
    fixture.tick(ticks);
    const KernelServerEntityState state = fixture.state();
    return KernelVec3{0.0f, state.position.y, state.velocity.y};
}

void the_patrol_path_falls_identically() {
    // Sequentially, never both at once: two live KernelHandles in one process
    // is not a configuration the game has, and it does not survive here.
    const KernelVec3 patrol = fall_after(7813, true, 10u);
    const KernelVec3 sentry = fall_after(7814, false, 10u);

    require(std::fabs(patrol.z - sentry.z) < 0.001f);
    require(std::fabs(patrol.y - sentry.y) < 0.001f);
}

}  // namespace

int main() {
    the_controller_no_longer_overwrites_vertical_velocity();
    a_sentry_spawned_in_the_air_falls();
    the_patrol_path_falls_identically();
    return 0;
}
