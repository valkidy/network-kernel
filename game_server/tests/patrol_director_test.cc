#include "game_server/src/patrol_director.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "game_server/src/agent_runtime.h"
#include "game_server/src/patrol_group_runtime.h"
#include "game_server/src/patrol_navigation.h"
#include "kernel/public/kernel_api.h"
#include "kernel/src/kernel_api_internal.h"

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

std::filesystem::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

std::vector<std::uint8_t> read_binary_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

constexpr std::uint32_t kTestFireActionTemplateId = 100;
constexpr std::uint32_t kTestReloadActionTemplateId = 101;
constexpr std::uint8_t kSpammerWeaponId =
    network_example::game_server::kAgentSpammerWeaponId;
constexpr std::uint32_t kGruntEntityTemplateId = 102;
constexpr std::uint32_t kBruteEntityTemplateId = 103;

using network_example::game_server::SpawnAreaShape;
using network_example::game_server::SpawnCompositionEntry;
using network_example::game_server::PatrolDefinitionConfig;

// The example from the original design note: a squad of 8 to 10 out of a mix
// authored as 2-8 of one and 4-20 of the other. Read as a second opinion about
// the total it is unsatisfiable -- the floors sum to 6 and the ceilings to 28 --
// which is exactly why the bands are floors plus capacity.
PatrolDefinitionConfig mixed_definition() {
    PatrolDefinitionConfig definition;
    definition.id = 1;
    definition.name = "mixed";
    definition.area.shape = SpawnAreaShape::kRect;
    definition.area.half_extents = KernelVec3{20.0f, 0.0f, 20.0f};
    definition.seed = 4242;
    definition.count_min = 8;
    definition.count_max = 10;
    definition.composition = {
        SpawnCompositionEntry{"grunt", kGruntEntityTemplateId, 2, 8},
        SpawnCompositionEntry{"brute", kBruteEntityTemplateId, 4, 20},
    };
    return definition;
}

void composition_is_floors_plus_capacity() {
    const PatrolDefinitionConfig definition = mixed_definition();
    require(network_example::game_server::validate_patrol_definition(definition)
                .empty());

    // Every total the count can draw is fillable, every floor is honoured, and
    // no ceiling is passed. Swept rather than sampled, because the failure this
    // guards against is one particular total coming out short.
    for (std::uint32_t count = definition.count_min;
         count <= definition.count_max;
         ++count) {
        for (std::uint32_t draw = 0; draw < 32; ++draw) {
            std::uint64_t state = draw * 0x9e3779b97f4a7c15ull + count;
            const std::vector<std::uint32_t> drawn =
                network_example::game_server::draw_spawn_composition(
                    definition.composition, count, &state);
            require(drawn.size() == definition.composition.size());
            std::uint32_t total = 0;
            for (std::size_t entry = 0; entry < drawn.size(); ++entry) {
                require(drawn[entry] >= definition.composition[entry].min_count);
                require(drawn[entry] <= definition.composition[entry].max_count);
                total += drawn[entry];
            }
            require(total == count);
        }
    }

    // And the mix actually varies. A draw that always answered "floors, then
    // everything to the first entry" would satisfy every assertion above.
    bool saw_more_grunts = false;
    bool saw_more_brutes = false;
    for (std::uint32_t draw = 0; draw < 64; ++draw) {
        std::uint64_t state = draw * 0x9e3779b97f4a7c15ull;
        const std::vector<std::uint32_t> drawn =
            network_example::game_server::draw_spawn_composition(definition.composition, 10, &state);
        saw_more_grunts = saw_more_grunts || drawn[0] > 4;
        saw_more_brutes = saw_more_brutes || drawn[1] > 6;
    }
    require(saw_more_grunts);
    require(saw_more_brutes);
}


// A narrow band beside a wide one must not saturate. The shipping warband is
// the shape that exposes it: 1-5 warriors and 6-24 rank and file over a total
// of 20-24 leaves a remainder of thirteen to seventeen, against four spare
// warrior slots. Handing that out uniformly among the entries with room filled
// the warriors to five about 95% of the time, which made `max: 5` read as
// "always 5". Weighted by remaining room it is roughly 20%.
//
// Stated as a band, not a figure: the exact rate is a property of the
// distribution, and pinning it would be pinning the PRNG stream instead.
void a_narrow_band_does_not_saturate_beside_a_wide_one() {
    PatrolDefinitionConfig definition = mixed_definition();
    definition.count_min = 20;
    definition.count_max = 24;
    definition.composition = {
        SpawnCompositionEntry{"warrior", kBruteEntityTemplateId, 1, 5},
        SpawnCompositionEntry{"rank_and_file", kGruntEntityTemplateId, 6, 24},
    };
    require(network_example::game_server::validate_patrol_definition(definition)
                .empty());

    constexpr int kDraws = 4000;
    int at_ceiling = 0;
    std::array<int, 6> warriors_seen{};
    for (int draw = 0; draw < kDraws; ++draw) {
        std::uint64_t state = static_cast<std::uint64_t>(draw) *
            0x9e3779b97f4a7c15ull;
        // The same uniform total the director draws.
        const std::uint32_t count = 20u + static_cast<std::uint32_t>(draw % 5);
        const std::vector<std::uint32_t> drawn =
            network_example::game_server::draw_spawn_composition(
                definition.composition, count, &state);
        require(drawn[0] >= 1u && drawn[0] <= 5u);
        require(drawn[0] + drawn[1] == count);
        ++warriors_seen[drawn[0]];
        at_ceiling += drawn[0] == 5u ? 1 : 0;
    }
    const double ceiling_rate =
        static_cast<double>(at_ceiling) / static_cast<double>(kDraws);
    require(ceiling_rate > 0.10);
    require(ceiling_rate < 0.32);
    // And the whole band is reachable, not just its ends.
    for (std::size_t warriors = 1; warriors <= 5; ++warriors) {
        require(warriors_seen[warriors] > 0);
    }
}

void an_unsatisfiable_definition_is_rejected() {
    using network_example::game_server::validate_patrol_definition;

    // Floors that do not fit inside the smallest squad the count can draw.
    PatrolDefinitionConfig too_many_floors = mixed_definition();
    too_many_floors.count_min = 5;
    require(!validate_patrol_definition(too_many_floors).empty());

    // Ceilings that cannot reach the largest.
    PatrolDefinitionConfig too_few_seats = mixed_definition();
    too_few_seats.composition[0].max_count = 2;
    too_few_seats.composition[1].max_count = 4;
    require(!validate_patrol_definition(too_few_seats).empty());

    PatrolDefinitionConfig empty_count = mixed_definition();
    empty_count.count_max = empty_count.count_min - 1;
    require(!validate_patrol_definition(empty_count).empty());

    PatrolDefinitionConfig no_composition = mixed_definition();
    no_composition.composition.clear();
    require(!validate_patrol_definition(no_composition).empty());

    // An area with no extent would place a whole squad on one point.
    PatrolDefinitionConfig flat_rect = mixed_definition();
    flat_rect.area.half_extents.z = 0.0f;
    require(!validate_patrol_definition(flat_rect).empty());

    PatrolDefinitionConfig no_radius = mixed_definition();
    no_radius.area.shape = SpawnAreaShape::kCircle;
    no_radius.area.half_extents.x = 0.0f;
    require(!validate_patrol_definition(no_radius).empty());
}

void a_formation_ranks_up_behind_the_squad() {
    const std::vector<KernelVec3> offsets =
        network_example::game_server::formation_offsets(7, 2.0f);
    require(offsets.size() == 7u);
    // Three across, then behind. Nobody stands in front of the squad's point.
    for (const KernelVec3& offset : offsets) {
        require(offset.x <= 0.0f);
    }
    require(offsets[0].z < offsets[1].z);
    require(offsets[1].z < offsets[2].z);
    require(offsets[3].x < offsets[0].x);
    // Nobody stands on anybody.
    for (std::size_t lhs = 0; lhs < offsets.size(); ++lhs) {
        for (std::size_t rhs = lhs + 1; rhs < offsets.size(); ++rhs) {
            require(
                std::fabs(offsets[lhs].x - offsets[rhs].x) > 0.01f ||
                std::fabs(offsets[lhs].z - offsets[rhs].z) > 0.01f);
        }
    }
}

KernelConfig server_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;
    config.max_events = 64;
    config.max_render_states = 64;
    return config;
}

KernelColliderTemplateDefinition hit_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 1;
    collider.shape_type = KernelColliderShapeType_Aabb;
    collider.center = KernelVec3{0.0f, 0.8f, 0.0f};
    collider.shape_params = KernelVec4{0.4f, 0.8f, 0.4f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Hit;
    collider.layer_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    return collider;
}

KernelColliderTemplateDefinition movement_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 11;
    collider.shape_type = KernelColliderShapeType_Capsule;
    collider.center = KernelVec3{0.0f, 0.9f, 0.0f};
    collider.shape_params = KernelVec4{0.55f, 0.35f, 0.0f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Movement;
    collider.layer_mask =
        KERNEL_COLLISION_LAYER_PLAYER_SIDE | KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
    return collider;
}

KernelColliderTemplateDefinition projectile_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 3;
    collider.shape_type = KernelColliderShapeType_Sphere;
    collider.shape_params = KernelVec4{0.2f, 0.0f, 0.0f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Hit;
    collider.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    return collider;
}

KernelProjectileTemplateDefinition projectile_template() {
    KernelProjectileTemplateDefinition projectile{};
    projectile.struct_size = sizeof(projectile);
    projectile.projectile_template_id = 3;
    projectile.weapon_id = kSpammerWeaponId;
    projectile.mechanics.struct_size = sizeof(KernelProjectileMechanicsDefinition);
    projectile.mechanics.projectile_type = KernelProjectileType_Standard;
    projectile.mechanics.motion_model = KernelProjectileMotionModel_Linear;
    projectile.mechanics.sync_mode = KernelProjectileSyncMode_ServerSnapshotOnly;
    projectile.mechanics.hit_response = KernelProjectileHitResponse_Destroy;
    projectile.mechanics.damage_shape = KernelProjectileDamageShape_DirectHit;
    projectile.mechanics.damage = 1;
    projectile.mechanics.speed = 35.0f;
    projectile.mechanics.lifetime_ticks = 90;
    projectile.mechanics.collider_template_id = 3;
    projectile.mechanics.collision_mask = KERNEL_COLLISION_MASK_DAMAGEABLE;
    projectile.mechanics.max_hit_count = 1;
    return projectile;
}

KernelActionTemplateDefinition fire_action_template() {
    KernelActionTemplateDefinition action{};
    action.struct_size = sizeof(action);
    action.action_template_id = kTestFireActionTemplateId;
    action.trigger_mode = KernelActionTriggerMode_Press;
    action.ammo_cost_per_commit = 1;
    action.max_commit_count = 1;
    return action;
}

KernelActionTemplateDefinition reload_action_template() {
    KernelActionTemplateDefinition action{};
    action.struct_size = sizeof(action);
    action.action_template_id = kTestReloadActionTemplateId;
    action.trigger_mode = KernelActionTriggerMode_Press;
    action.commit_offset_ticks = 3;
    action.max_commit_count = 1;
    return action;
}

KernelColliderTemplateDefinition vision_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 2;
    collider.shape_type = KernelColliderShapeType_Cone;
    collider.shape_params = KernelVec4{20.0f, 90.0f, 0.0f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Vision;
    collider.layer_mask = KERNEL_COLLISION_LAYER_AGENT_VISION;
    return collider;
}

// Trimmed from agent_chaser_controller_test's: no weapon, because nothing here
// fires. Vision stays -- an agent template carrying the sentry runtime has to
// have somewhere to look from -- and so does the component set, which is what
// makes an entity an agent the runtime will pick up.
KernelEntityTemplateDefinition agent_entity_template(std::uint32_t template_id) {
    KernelEntityTemplateDefinition entity_template{};
    entity_template.struct_size = sizeof(entity_template);
    entity_template.entity_template_id = template_id;
    entity_template.entity_type = KernelEntityType_Actor;
    entity_template.actor_type = KernelActorType_Agent;
    entity_template.actor_template_id = 2u;
    entity_template.component_flags =
        KERNEL_ENTITY_COMPONENT_TRANSFORM | KERNEL_ENTITY_COMPONENT_VELOCITY |
        KERNEL_ENTITY_COMPONENT_HEALTH | KERNEL_ENTITY_COMPONENT_HITBOX |
        KERNEL_ENTITY_COMPONENT_AGENT_RUNTIME |
        KERNEL_ENTITY_COMPONENT_SENTRY_RUNTIME;
    entity_template.collider_template_id = 1u;
    entity_template.combat.struct_size = sizeof(entity_template.combat);
    entity_template.combat.hp = 100;
    entity_template.combat.max_hp = 100;
    entity_template.combat.move_speed_meters_per_second = 2.5f;
    entity_template.combat.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    entity_template.combat.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    entity_template.ai.struct_size = sizeof(KernelEntityAiDefinition);
    entity_template.movement.struct_size = sizeof(KernelMovementDefinition);
    entity_template.movement.controller_type =
        KernelMovementControllerType_Grounded;
    entity_template.movement.movement_collider_template_id = 11u;
    entity_template.movement.max_slope_degrees = 50.0f;
    entity_template.movement.ground_probe_distance = 0.25f;
    entity_template.movement.ground_snap_distance = 0.5f;
    entity_template.ai.controller_type = KernelAiControllerType_Chaser;
    entity_template.ai.tick_interval = 1u;
    entity_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
    entity_template.vision.camp = KernelAgentCamp_EnemySide;
    entity_template.vision.vision_collider_template_id = 2u;
    entity_template.vision.max_visible_hostiles = KERNEL_MAX_VISIBLE_HOSTILES;
    entity_template.vision.max_visible_allies = KERNEL_MAX_VISIBLE_ALLIES;
    return entity_template;
}

KernelActorTemplateDefinition agent_actor_template() {
    KernelActorTemplateDefinition actor_template{};
    actor_template.struct_size = sizeof(actor_template);
    actor_template.actor_template_id = 2u;
    actor_template.entity_type = KernelEntityType_Actor;
    actor_template.actor_type = KernelActorType_Agent;
    actor_template.collider_template_id = 1u;
    actor_template.vision.struct_size = sizeof(KernelAgentVisionConfig);
    actor_template.vision.camp = KernelAgentCamp_EnemySide;
    actor_template.vision.vision_collider_template_id = 2u;
    actor_template.vision.max_visible_hostiles = KERNEL_MAX_VISIBLE_HOSTILES;
    actor_template.vision.max_visible_allies = KERNEL_MAX_VISIBLE_ALLIES;
    return actor_template;
}

void load_catalog(KernelHandle* kernel) {
    const std::array<KernelColliderTemplateDefinition, 4> colliders = {
        hit_collider_template(),
        vision_collider_template(),
        projectile_collider_template(),
        movement_collider_template(),
    };
    const KernelProjectileTemplateDefinition projectile = projectile_template();
    const std::array<KernelActionTemplateDefinition, 2> actions = {
        fire_action_template(),
        reload_action_template(),
    };
    const std::array<KernelEntityTemplateDefinition, 2> entity_templates = {
        agent_entity_template(kGruntEntityTemplateId),
        agent_entity_template(kBruteEntityTemplateId),
    };
    const KernelActorTemplateDefinition actor_template = agent_actor_template();
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 1;
    catalog.collider_templates = colliders.data();
    catalog.collider_template_count =
        static_cast<std::uint32_t>(colliders.size());
    catalog.projectile_templates = &projectile;
    catalog.projectile_template_count = 1;
    catalog.action_templates = actions.data();
    catalog.action_template_count = static_cast<std::uint32_t>(actions.size());
    catalog.actor_templates = &actor_template;
    catalog.actor_template_count = 1;
    catalog.entity_templates = entity_templates.data();
    catalog.entity_template_count =
        static_cast<std::uint32_t>(entity_templates.size());
    require(Kernel_LoadGameplayCatalog(kernel, &catalog, nullptr));
}

// The director spawns on its interval, once, into a squad whose members are
// real entities standing in the authored area.
void a_patrol_spawns_on_its_interval() {
    using network_example::game_server::PatrolDirector;
    using network_example::game_server::PatrolGroupRuntime;

    const KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7823));
    load_catalog(kernel);

    PatrolDefinitionConfig definition = mixed_definition();
    definition.interval_ticks = 5;
    definition.max_live_groups = 1;
    PatrolDirector director({definition});
    PatrolGroupRuntime groups;

    // Staggered by one interval rather than firing on tick zero.
    for (std::uint32_t tick = 0; tick < definition.interval_ticks; ++tick) {
        director.tick(kernel, &groups, nullptr);
        require(groups.groups().empty());
    }
    director.tick(kernel, &groups, nullptr);
    require(groups.groups().size() == 1u);
    require(director.spawned_group_count() == 1u);

    const network_example::game_server::PatrolGroup& group = groups.groups()[0];
    require(group.definition_id == definition.id);
    require(group.member_net_ids.size() >= definition.count_min);
    require(group.member_net_ids.size() <= definition.count_max);
    require(group.member_offsets.size() == group.member_net_ids.size());
    // One waypoint: the cursor starts where the squad spawned, so the straight
    // chord is the single leg to the far end.
    require(group.waypoints.size() == 1u);

    // Every member is a live entity, inside the authored rectangle, and no two
    // of them were put in the same place.
    for (std::size_t member = 0; member < group.member_net_ids.size(); ++member) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        require(Kernel_ServerGetEntityState(
            kernel, group.member_net_ids[member], &state));
        require(state.valid != 0u);
        require(state.actor_type == KernelActorType_Agent);
        // The formation reaches outside the area by its own spacing, which is
        // why this is the extent plus a formation's width rather than the
        // extent alone.
        require(std::fabs(state.position.x) <= 20.0f + 8.0f);
        require(std::fabs(state.position.z) <= 20.0f + 8.0f);
        for (std::size_t other = member + 1;
             other < group.member_net_ids.size();
             ++other) {
            require(group.member_net_ids[member] != group.member_net_ids[other]);
        }
    }

    // The ceiling holds: the interval passes again and no second squad appears
    // while the first is still alive.
    for (std::uint32_t tick = 0; tick < definition.interval_ticks * 3u; ++tick) {
        director.tick(kernel, &groups, nullptr);
    }
    require(groups.groups().size() == 1u);
    require(director.spawned_group_count() == 1u);

    Kernel_Destroy(kernel);
}

// Two directors built from the same definition spawn the same squad. The draws
// are derived from the seed and the count of squads already spawned, which is
// what lets a patrol be reasoned about before it exists.
void the_same_seed_spawns_the_same_squad() {
    using network_example::game_server::PatrolDirector;
    using network_example::game_server::PatrolGroupRuntime;

    PatrolDefinitionConfig definition = mixed_definition();
    definition.interval_ticks = 1;
    definition.max_live_groups = 4;

    std::vector<std::size_t> sizes;
    std::vector<float> first_waypoints;
    for (int run = 0; run < 2; ++run) {
        const KernelConfig config = server_config();
        KernelHandle* kernel = Kernel_Create(&config);
        require(kernel != nullptr);
        require(Kernel_StartDedicatedServer(
            kernel, static_cast<std::uint16_t>(7824 + run)));
        load_catalog(kernel);
        PatrolDirector director({definition});
        PatrolGroupRuntime groups;
        for (int tick = 0; tick < 8; ++tick) {
            director.tick(kernel, &groups, nullptr);
        }
        require(groups.groups().size() == 4u);
        for (const network_example::game_server::PatrolGroup& group :
             groups.groups()) {
            sizes.push_back(group.member_net_ids.size());
            first_waypoints.push_back(group.waypoints[0].x);
        }
        Kernel_Destroy(kernel);
    }
    require(sizes.size() == 8u);
    for (std::size_t index = 0; index < 4; ++index) {
        require(sizes[index] == sizes[index + 4]);
        require(first_waypoints[index] == first_waypoints[index + 4]);
    }
    // Successive squads out of one definition differ, or the seed would be
    // producing one squad over and over rather than a replayable sequence.
    bool varied = false;
    for (std::size_t index = 1; index < 4; ++index) {
        varied = varied || first_waypoints[index] != first_waypoints[0];
    }
    require(varied);
}


// Walks until the squad's route is finished, and no further -- the linger
// starts counting the moment it is, so overshooting here would spend it.
void walk_until_route_complete(
    network_example::game_server::PatrolGroupRuntime* groups,
    std::vector<network_example::game_server::AgentRuntimeState>* agents) {
    for (int tick = 0; tick < 500; ++tick) {
        if (!groups->groups().empty() && groups->groups()[0].route_complete) {
            return;
        }
        groups->tick(agents, 1.0f / 30.0f);
    }
}

// Walks a squad's route out by ticking the group runtime, which is what moves
// the cursor and starts the retirement linger counting.
void walk_route_out(
    network_example::game_server::PatrolGroupRuntime* groups,
    std::vector<network_example::game_server::AgentRuntimeState>* agents,
    int ticks) {
    for (int tick = 0; tick < ticks; ++tick) {
        groups->tick(agents, 1.0f / 30.0f);
    }
}

std::vector<network_example::game_server::AgentRuntimeState> members_of(
    const network_example::game_server::PatrolGroup& group) {
    std::vector<network_example::game_server::AgentRuntimeState> agents;
    for (const std::uint32_t net_id : group.member_net_ids) {
        network_example::game_server::AgentRuntimeState agent;
        agent.net_id = net_id;
        agents.push_back(agent);
    }
    return agents;
}

// A finished squad goes, and its entities go with it. Without this a
// definition's live ceiling fills with squads standing at the end of their
// route and never spawns again -- which is what the previous phase shipped.
void a_finished_patrol_retires_after_its_linger() {
    using network_example::game_server::AgentSentryState;
    using network_example::game_server::PatrolDirector;
    using network_example::game_server::PatrolGroupRuntime;

    const KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7826));
    load_catalog(kernel);

    PatrolDefinitionConfig definition = mixed_definition();
    definition.interval_ticks = 1;
    definition.max_live_groups = 1;
    definition.despawn_linger_ticks = 10;
    // Fast enough that walking the route out is a handful of ticks rather than
    // the three quarters of a minute the shipping cadence takes.
    definition.group.advance_speed_meters_per_second = 200.0f;
    PatrolDirector director({definition});
    PatrolGroupRuntime groups;

    director.tick(kernel, &groups, nullptr);
    director.tick(kernel, &groups, nullptr);
    require(groups.groups().size() == 1u);
    const std::vector<std::uint32_t> members = groups.groups()[0].member_net_ids;
    std::vector<network_example::game_server::AgentRuntimeState> agents =
        members_of(groups.groups()[0]);

    // Finished, but still inside the linger: it stays.
    walk_until_route_complete(&groups, &agents);
    require(groups.groups()[0].route_complete);
    require(
        groups.groups()[0].ticks_since_route_complete <
        definition.despawn_linger_ticks);
    director.tick(kernel, &groups, nullptr);
    require(groups.groups().size() == 1u);
    require(director.retired_group_count() == 0u);

    walk_route_out(&groups, &agents, 20);
    director.tick(kernel, &groups, nullptr);
    Kernel_Update(kernel, 1.0f / 30.0f);
    require(director.retired_group_count() == 1u);

    // The entities are actually gone, not just the bookkeeping.
    for (const std::uint32_t net_id : members) {
        KernelServerEntityState state{};
        state.struct_size = sizeof(state);
        const bool found = Kernel_ServerGetEntityState(kernel, net_id, &state);
        require(!found || state.valid == 0u);
    }

    // And the freed place is taken, which is the whole point of retiring.
    director.tick(kernel, &groups, nullptr);
    require(groups.groups().size() == 1u);
    require(director.spawned_group_count() == 2u);

    Kernel_Destroy(kernel);
}

// Despawning enemies out from under the player shooting at them is worse than
// any population it would have saved, so a squad in a fight is never retired.
//
// The squad is walked past its linger BEFORE the fight starts, so retirement is
// due and the engagement is the only thing holding it off. Starting the fight
// first would freeze the route instead, and the case would pass without the
// guard existing at all.
void a_squad_in_a_fight_is_never_retired() {
    using network_example::game_server::AgentSentryState;
    using network_example::game_server::PatrolDirector;
    using network_example::game_server::PatrolGroupRuntime;

    const KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7827));
    load_catalog(kernel);

    PatrolDefinitionConfig definition = mixed_definition();
    definition.interval_ticks = 1;
    definition.max_live_groups = 1;
    definition.despawn_linger_ticks = 5;
    definition.group.advance_speed_meters_per_second = 200.0f;
    PatrolDirector director({definition});
    PatrolGroupRuntime groups;

    director.tick(kernel, &groups, nullptr);
    director.tick(kernel, &groups, nullptr);
    require(groups.groups().size() == 1u);
    std::vector<network_example::game_server::AgentRuntimeState> agents =
        members_of(groups.groups()[0]);

    walk_until_route_complete(&groups, &agents);
    walk_route_out(&groups, &agents, 20);
    require(
        groups.groups()[0].ticks_since_route_complete >=
        definition.despawn_linger_ticks);

    // Retirement is due, and then one of them picks a fight.
    agents[0].sentry.state = AgentSentryState::kAlert;
    groups.tick(&agents, 1.0f / 30.0f);
    require(groups.groups()[0].holding);
    for (int tick = 0; tick < 10; ++tick) {
        director.tick(kernel, &groups, nullptr);
    }
    require(director.retired_group_count() == 0u);
    require(groups.groups().size() == 1u);

    // The fight ends, and now it goes.
    agents[0].sentry.state = AgentSentryState::kIdle;
    groups.tick(&agents, 1.0f / 30.0f);
    director.tick(kernel, &groups, nullptr);
    require(director.retired_group_count() == 1u);

    Kernel_Destroy(kernel);
}

std::uint32_t create_player(KernelHandle* kernel, const KernelVec3& position) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = network_example::game_server::kEntityTypeActor;
    create_info.actor_type = network_example::game_server::kActorTypePlayer;
    create_info.position = position;
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    require(net_id != 0);
    return net_id;
}

// The distance rule retires a squad nobody is near, and only that squad. Its
// route is nowhere near finished in either half here, so the linger cannot be
// what decides it.
void a_patrol_nobody_is_near_retires_early() {
    using network_example::game_server::PatrolDirector;
    using network_example::game_server::PatrolGroupRuntime;

    const KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7830));
    load_catalog(kernel);

    PatrolDefinitionConfig definition = mixed_definition();
    definition.interval_ticks = 1;
    definition.max_live_groups = 1;
    definition.despawn_linger_ticks = 100000;
    definition.despawn_distance_meters = 50.0f;

    // A player standing on the area keeps the squad, which is the half that
    // says the rule is reading a distance rather than firing on any player at
    // all.
    create_player(kernel, KernelVec3{0.0f, 0.0f, 0.0f});
    PatrolDirector near_director({definition});
    PatrolGroupRuntime near_groups;
    near_director.tick(kernel, &near_groups, nullptr);
    near_director.tick(kernel, &near_groups, nullptr);
    require(near_groups.groups().size() == 1u);
    for (int tick = 0; tick < 10; ++tick) {
        near_director.tick(kernel, &near_groups, nullptr);
    }
    require(near_director.retired_group_count() == 0u);

    // The same squad, with the only player 500 m away.
    create_player(kernel, KernelVec3{500.0f, 0.0f, 500.0f});
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    PatrolDirector far_director({definition});
    PatrolGroupRuntime far_groups;
    far_director.tick(kernel, &far_groups, nullptr);
    far_director.tick(kernel, &far_groups, nullptr);
    require(far_groups.groups().size() == 1u);
    // The near player has to go first, or the nearest one is still on the area.
    KernelEntityLifecycleCommand destroy{};
    destroy.struct_size = sizeof(destroy);
    destroy.command_type = KernelEntityLifecycleCommandType_Destroy;
    destroy.net_id = 1;
    destroy.reason = 0;
    Kernel_ServerEnqueueEntityLifecycle(
        kernel, KernelCommandSource_Internal, &destroy);
    Kernel_Update(kernel, 1.0f / 30.0f);
    far_director.tick(kernel, &far_groups, nullptr);
    require(far_director.retired_group_count() == 1u);

    Kernel_Destroy(kernel);
}

// An empty server is not "everyone is infinitely far away". Reading it that way
// would have a server with nobody on it sweep away every patrol it spawns, on
// the tick it spawns them.
void an_empty_server_does_not_sweep_its_patrols_away() {
    using network_example::game_server::PatrolDirector;
    using network_example::game_server::PatrolGroupRuntime;

    const KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7828));
    load_catalog(kernel);

    PatrolDefinitionConfig definition = mixed_definition();
    definition.interval_ticks = 1;
    definition.max_live_groups = 1;
    definition.despawn_linger_ticks = 100000;
    definition.despawn_distance_meters = 1.0f;
    PatrolDirector director({definition});
    PatrolGroupRuntime groups;

    director.tick(kernel, &groups, nullptr);
    director.tick(kernel, &groups, nullptr);
    require(groups.groups().size() == 1u);
    for (int tick = 0; tick < 20; ++tick) {
        director.tick(kernel, &groups, nullptr);
    }
    require(director.retired_group_count() == 0u);
    require(groups.groups().size() == 1u);

    Kernel_Destroy(kernel);
}

// Per-definition ceilings cannot express a total: two definitions of four
// squads each are eight squads, and only the budget notices.
void the_budget_caps_agents_across_definitions() {
    using network_example::game_server::PatrolBudgetConfig;
    using network_example::game_server::PatrolDirector;
    using network_example::game_server::PatrolGroupRuntime;

    const KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7829));
    load_catalog(kernel);

    PatrolDefinitionConfig first = mixed_definition();
    first.interval_ticks = 1;
    first.max_live_groups = 4;
    PatrolDefinitionConfig second = mixed_definition();
    second.id = 2;
    second.name = "mixed_two";
    second.interval_ticks = 1;
    second.max_live_groups = 4;

    PatrolBudgetConfig budget;
    // Two squads' worth at the largest they can draw, so the ceilings would
    // allow eight squads and the budget allows two.
    budget.max_live_agents = 20;
    PatrolDirector director({first, second}, budget);
    PatrolGroupRuntime groups;

    for (int tick = 0; tick < 40; ++tick) {
        director.tick(kernel, &groups, nullptr);
        std::uint32_t live = 0;
        for (const network_example::game_server::PatrolGroup& group :
             groups.groups()) {
            live += static_cast<std::uint32_t>(group.member_net_ids.size());
        }
        require(live <= budget.max_live_agents);
    }
    require(groups.groups().size() == 2u);
    // Both definitions got a look in, rather than the first eating the budget.
    require(groups.groups()[0].definition_id != groups.groups()[1].definition_id);

    Kernel_Destroy(kernel);
}


// With a navmesh, a route is a path: it bends around what is in the way, and
// its start is somewhere the squad can stand. The straight chord this replaces
// can do neither, so a bend is the observable difference.
void a_navigable_patrol_routes_around_obstacles() {
    using network_example::game_server::PatrolDirector;
    using network_example::game_server::PatrolGroupRuntime;
    using network_example::game_server::PatrolNavigation;

    const std::vector<std::uint8_t> artifact = read_binary_file(
        (runfiles_root() / "game_server" / "test_mesh_assets" / "generated" /
         "recast" / "obstructed_field.navmesh")
            .string());
    PatrolNavigation navigation;
    std::string load_error;
    require(navigation.load(artifact, &load_error));

    const KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7831));
    load_catalog(kernel);

    PatrolDefinitionConfig definition = mixed_definition();
    definition.interval_ticks = 1;
    definition.max_live_groups = 16;
    // The whole terrain, walls and pits included, so draws land on both sides
    // of the zigzag.
    definition.area.half_extents = KernelVec3{45.0f, 0.0f, 45.0f};
    definition.max_detour_ratio = 6.0f;
    definition.route_attempts = 16;
    PatrolDirector director({definition}, {});
    PatrolGroupRuntime groups;

    for (int tick = 0; tick < 40; ++tick) {
        director.tick(kernel, &groups, &navigation);
    }
    require(groups.groups().size() == 16u);

    bool saw_a_bend = false;
    for (const network_example::game_server::PatrolGroup& group :
         groups.groups()) {
        require(!group.waypoints.empty());
        saw_a_bend = saw_a_bend || group.waypoints.size() > 1u;
        // Every squad starts where it can stand. A draw that landed in a pit
        // was either snapped out of it or thrown away.
        KernelVec3 snapped{0.0f, 0.0f, 0.0f};
        require(navigation.snap(group.cursor, 0.5f, &snapped));
    }
    require(saw_a_bend);

    Kernel_Destroy(kernel);
}

// An area with no walkable ground in it produces no squads, and says so,
// rather than producing squads standing inside the scenery.
void an_unwalkable_area_spawns_nothing() {
    using network_example::game_server::PatrolDirector;
    using network_example::game_server::PatrolGroupRuntime;
    using network_example::game_server::PatrolNavigation;

    const std::vector<std::uint8_t> artifact = read_binary_file(
        (runfiles_root() / "game_server" / "test_mesh_assets" / "generated" /
         "recast" / "obstructed_field.navmesh")
            .string());
    PatrolNavigation navigation;
    std::string load_error;
    require(navigation.load(artifact, &load_error));

    const KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, 7832));
    load_catalog(kernel);

    PatrolDefinitionConfig definition = mixed_definition();
    definition.interval_ticks = 1;
    definition.max_live_groups = 4;
    // Well off the terrain entirely.
    definition.area.center = KernelVec3{5000.0f, 0.0f, 5000.0f};
    definition.area.half_extents = KernelVec3{10.0f, 0.0f, 10.0f};
    definition.route_attempts = 4;
    PatrolDirector director({definition}, {});
    PatrolGroupRuntime groups;

    for (int tick = 0; tick < 20; ++tick) {
        director.tick(kernel, &groups, &navigation);
    }
    require(groups.groups().empty());
    require(director.route_failure_count() > 0u);
    require(director.spawned_group_count() == 0u);

    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    composition_is_floors_plus_capacity();
    a_narrow_band_does_not_saturate_beside_a_wide_one();
    an_unsatisfiable_definition_is_rejected();
    a_formation_ranks_up_behind_the_squad();
    a_patrol_spawns_on_its_interval();
    the_same_seed_spawns_the_same_squad();
    a_finished_patrol_retires_after_its_linger();
    a_squad_in_a_fight_is_never_retired();
    an_empty_server_does_not_sweep_its_patrols_away();
    a_patrol_nobody_is_near_retires_early();
    the_budget_caps_agents_across_definitions();
    a_navigable_patrol_routes_around_obstacles();
    an_unwalkable_area_spawns_nothing();
    return 0;
}
