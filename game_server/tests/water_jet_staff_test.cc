// The Water Jet Staff end to end (docs/WATER_JET_PLAN.md, P4): a player armed
// with the shipped staff through a weapon container fires its bolt through the
// real input path at the shipped static scene, and a gingerbread standing
// where the bolt lands is shoved off the face the bolt struck -- and actually
// travels, rather than only getting a velocity for a tick.
//
//   - at the open ground: thrown straight up;
//   - at a slope, and at a tree trunk: lifted and carried out from the face.
//
// The push is the column's split impulse, (n.x * h, v, n.z * h) for the face
// normal n, with h and v read from the catalog. A flat push on a grounded
// actor moved it 0.40 m and died the next tick; the lift is what makes it
// carry, which is why the travel is checked and not only the direction.
//
// The face is not assumed. This test loads the same scene into a physics
// world of its own, casts the bolt's own path -- read off the live bolt -- and
// takes the face normal that ray returns; the push is held to that. The
// cliffs are rough, roughly 67-degree slopes, so no fixed vector would do.
//
// Each case fires twice from the same place along the same aim: the first
// shot, with nobody near, finds where the bolt lands; the gingerbread is then
// put there and the second shot shoves it. The bolt strikes only the world, so
// it flies through the gingerbread to the same spot.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"
#include "physics/public/physics_world.h"

namespace {

namespace fs = std::filesystem;
namespace gs = network_example::game_server;
namespace physics = network_example::physics;

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr float kGravity = 9.81f;

// The column's push, as the catalog compiles it.
struct Push {
    float horizontal = 0.0f;
    float vertical = 0.0f;

    glm::vec3 delta(const glm::vec3& normal) const {
        return {normal.x * horizontal, vertical, normal.z * horizontal};
    }
};

fs::path runfiles_catalog() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / "game_server" / "gameplay_catalog";
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

glm::vec3 to_glm(const KernelVec3& value) { return {value.x, value.y, value.z}; }
KernelVec3 to_kernel(const glm::vec3& value) { return {value.x, value.y, value.z}; }

float angle_degrees(const glm::vec3& a, const glm::vec3& b) {
    return std::acos(std::clamp(
               glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f)) *
        57.2957795f;
}

KernelServerEntityState entity_state(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    require(Kernel_ServerGetEntityState(kernel, net_id, &state));
    return state;
}

// The scene the kernel has, as a physics world this test can cast into.
struct Scene {
    physics::PhysicsWorld world;
    std::uint32_t mask =
        physics::collision_layer_bit(physics::CollisionLayer::kTerrain) |
        physics::collision_layer_bit(physics::CollisionLayer::kStaticObstacle);

    bool cast(const glm::vec3& origin, const glm::vec3& direction, float distance,
              physics::CollisionHit* hit) const {
        physics::RayCastRequest ray{};
        ray.origin = origin;
        ray.direction = glm::normalize(direction);
        ray.max_distance = distance;
        ray.filter.collision_mask = mask;
        return world.ray_cast_closest(ray, hit);
    }

    float ground(float x, float z) const {
        physics::CollisionHit hit{};
        require(cast({x, 60.0f, z}, {0.0f, -1.0f, 0.0f}, 120.0f, &hit));
        return hit.position.y;
    }
};

struct Bolt {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
};

struct Arena {
    KernelHandle* kernel = nullptr;
    std::uint8_t weapon = 0;
    std::uint32_t player = 0;
    std::uint32_t next_seq = 1;
    std::uint32_t next_action = 1;

    ~Arena() {
        if (kernel != nullptr) Kernel_Destroy(kernel);
    }

    void send(const glm::vec3& aim, std::uint32_t action, bool start, bool held) {
        KernelPlayerInput input{};
        input.input_seq = next_seq++;
        input.aim_dir = to_kernel(glm::normalize(aim));
        input.selected_weapon = weapon;
        if (action != 0u) {
            if (start) {
                input.action_intent =
                    KernelActionIntent{action, KernelActionBinding_PrimaryFire, 0u, 0u};
            }
            input.action_input =
                KernelActionInput{action, static_cast<std::uint8_t>(held ? 1u : 0u), 0u, 0u};
        }
        require(Kernel_ServerSubmitEntityInput(kernel, player, &input));
        Kernel_Update(kernel, kTickSeconds);
    }

    void idle(const glm::vec3& aim, int ticks) {
        for (int index = 0; index < ticks; ++index) send(aim, 0u, false, false);
    }

    std::vector<KernelServerEntityState> projectiles() const {
        std::vector<KernelServerEntityState> found(16);
        for (auto& state : found) state.struct_size = sizeof(state);
        const std::uint32_t count = Kernel_ServerQueryEntities(
            kernel, KernelEntityType_Projectile, found.data(),
            static_cast<std::uint32_t>(found.size()));
        found.resize(std::min<std::size_t>(count, found.size()));
        return found;
    }

    // One press, held to the first commit: one bolt. Returns it as it was on
    // the first tick it existed.
    Bolt fire(const glm::vec3& aim, int commit_offset) {
        require(projectiles().empty());
        const std::uint32_t action = 9200u + next_action++;
        std::vector<Bolt> seen;
        for (int index = 0; index <= commit_offset + 1; ++index) {
            send(aim, action, index == 0, index <= commit_offset);
            for (const KernelServerEntityState& state : projectiles()) {
                if (seen.empty()) {
                    seen.push_back({to_glm(state.position), to_glm(state.velocity)});
                }
            }
        }
        require(seen.size() == 1u);
        return seen.front();
    }

    void wait_for_no_projectiles(const glm::vec3& aim) {
        for (int tick = 0; tick < 120 && !projectiles().empty(); ++tick) idle(aim, 1);
        require(projectiles().empty());
    }
};

struct Case {
    const char* name;
    glm::vec3 shooter;
    glm::vec3 aim;
    // What kind of face the bolt must strike, by the normal's y: a floor near
    // 1, a slope between, a wall near 0.
    float min_normal_y;
    float max_normal_y;
    // How far out it must at least be carried, whatever the tuning: the
    // expected carry follows the catalog, so a lift tuned near zero would
    // shrink the expectation with it and still pass.
    float min_carry;
};

// Fires the case's two shots and holds the push the gingerbread got to the
// face the bolt struck.
void run_case(Arena& arena, const Scene& scene, const Case& c,
              std::uint32_t gingerbread, int commit_offset, float bolt_range,
              const Push& column_push) {
    const KernelServerEntityState player = entity_state(arena.kernel, arena.player);
    const KernelVec3 at = to_kernel(c.shooter);
    require(Kernel_ServerSetEntityTransform(
        arena.kernel, arena.player, &at, &player.rotation));
    arena.idle(c.aim, 30);

    // Shot one: where does it land, and on what face?
    const Bolt dry = arena.fire(c.aim, commit_offset);
    require(glm::length(dry.velocity) > 1.0f);
    const glm::vec3 heading = glm::normalize(dry.velocity);
    const glm::vec3 from = dry.position - dry.velocity * (2.0f * kTickSeconds);
    physics::CollisionHit face{};
    require(scene.cast(from, heading, bolt_range + 5.0f, &face));
    arena.wait_for_no_projectiles(c.aim);
    std::fprintf(stderr,
                 "water_jet[%s]: bolt from (%.2f %.2f %.2f) lands (%.2f %.2f %.2f), "
                 "face normal (%.2f %.2f %.2f)\n",
                 c.name, from.x, from.y, from.z, face.position.x, face.position.y,
                 face.position.z, face.normal.x, face.normal.y, face.normal.z);
    require(face.normal.y >= c.min_normal_y && face.normal.y <= c.max_normal_y);

    // Shot two, with a gingerbread standing just off the face: half a metre
    // out along the horizontal part of the normal, on the ground.
    glm::vec3 out{face.normal.x, 0.0f, face.normal.z};
    out = glm::length(out) > 0.1f ? glm::normalize(out) : glm::vec3{0.0f};
    glm::vec3 stand = face.position + out * 0.8f;
    stand.y = scene.ground(stand.x, stand.z);
    KernelServerEntityCreateInfo agent{};
    agent.struct_size = sizeof(agent);
    agent.entity_type = KernelEntityType_Actor;
    agent.actor_type = KernelActorType_Agent;
    agent.entity_template_id = gingerbread;
    agent.actor_template_id = gingerbread;
    agent.position = to_kernel(stand + glm::vec3{0.0f, 0.1f, 0.0f});
    agent.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t target = 0;
    require(Kernel_ServerCreateEntity(arena.kernel, &agent, &target));
    require(Kernel_ServerSetEntityActorTemplate(arena.kernel, target, gingerbread));
    arena.idle(c.aim, 20);
    const glm::vec3 rest = to_glm(entity_state(arena.kernel, target).position);
    require(glm::length(to_glm(entity_state(arena.kernel, target).velocity)) < 0.5f);

    const Bolt wet = arena.fire(c.aim, commit_offset);
    require(glm::length(wet.position - dry.position) < 0.01f);
    // The first tick the gingerbread moves fast is the push; gravity has had
    // at most a tick at it.
    glm::vec3 push{0.0f};
    for (int tick = 0; tick < 60 && glm::length(push) < 5.0f; ++tick) {
        arena.idle(c.aim, 1);
        push = to_glm(entity_state(arena.kernel, target).velocity);
    }
    const glm::vec3 expected = column_push.delta(face.normal);
    const float error = angle_degrees(push, expected);
    std::fprintf(stderr,
                 "water_jet[%s]: gingerbread at (%.2f %.2f %.2f) pushed (%.2f %.2f %.2f), "
                 "expected (%.2f %.2f %.2f), %.2f deg off\n",
                 c.name, rest.x, rest.y, rest.z, push.x, push.y, push.z,
                 expected.x, expected.y, expected.z, error);
    require(glm::length(push) >= 5.0f);
    // One tick of gravity bends it under 2 degrees.
    require(error < 5.0f);

    // Then where it goes: up, out, and down again. On flat ground the lift
    // keeps it up 2v/g, and the horizontal part carries it that long.
    const float airtime = 2.0f * column_push.vertical / kGravity;
    const float expected_peak =
        column_push.vertical * column_push.vertical / (2.0f * kGravity);
    const float expected_carry =
        glm::length(glm::vec2{expected.x, expected.z}) * airtime;
    float peak = 0.0f;
    glm::vec3 last = to_glm(entity_state(arena.kernel, target).position);
    int still_ticks = 0;
    for (int tick = 0; tick < 150 && still_ticks < 5; ++tick) {
        arena.idle(c.aim, 1);
        const KernelServerEntityState state = entity_state(arena.kernel, target);
        last = to_glm(state.position);
        peak = std::max(peak, last.y - rest.y);
        still_ticks = glm::length(to_glm(state.velocity)) < 0.05f ? still_ticks + 1 : 0;
    }
    const float carry = glm::length(glm::vec2{last.x - rest.x, last.z - rest.z});
    std::fprintf(stderr,
                 "water_jet[%s]: rose %.2f m (expected %.2f), carried %.2f m "
                 "(expected %.2f), came to rest %s\n",
                 c.name, peak, expected_peak, carry, expected_carry,
                 still_ticks >= 5 ? "yes" : "no");
    require(still_ticks >= 5);
    require(peak > 0.7f * expected_peak);
    require(carry >= c.min_carry);
    if (expected_carry > 1.0f) {
        // Out from the face, not just anywhere: most of the way it should go.
        require(carry > 0.6f * expected_carry);
        require(glm::dot(glm::vec2{last.x - rest.x, last.z - rest.z},
                         glm::vec2{expected.x, expected.z}) > 0.0f);
    } else {
        require(carry < 0.5f);
    }
    require(Kernel_ServerDestroyEntity(arena.kernel, target, 0u));
    arena.wait_for_no_projectiles(c.aim);
}

}  // namespace

int main() {
    const gs::GameServerGameplayConfig config = gs::default_game_server_gameplay_config();
    std::uint32_t bolt_id = 0;
    const KernelProjectileMechanicsDefinition* bolt = nullptr;
    for (const auto& projectile : config.projectile_templates) {
        if (projectile.name == "water_jet_bolt") {
            bolt_id = projectile.definition.projectile_template_id;
            bolt = &projectile.definition.mechanics;
        }
    }
    std::uint32_t staff_item = 0;
    for (const auto& item : config.item_templates) {
        if (item.name == "stateful_weapon_water_jet_staff") {
            staff_item = item.definition.item_template_id;
        }
    }
    std::uint32_t gingerbread = 0;
    for (const auto& entity : config.entity_templates) {
        if (entity.name == "gingerbread") gingerbread = entity.actor_template_id;
    }
    require(bolt != nullptr && staff_item != 0u && gingerbread != 0u);
    Push column_push;
    for (const auto& projectile : config.projectile_templates) {
        if (projectile.name != "water_jet_column") continue;
        const KernelActionDefinition& action =
            projectile.definition.mechanics.projectile_impact_trigger.actions[0];
        require(action.action_type == KernelEntityTriggerActionType_ApplyImpulse);
        require(action.impulse_strength_mode == KERNEL_IMPULSE_STRENGTH_MODE_SPLIT);
        column_push.horizontal = action.impulse_strength;
        column_push.vertical = action.impulse_strength_vertical;
    }
    require(column_push.vertical > 0.0f);
    std::uint8_t weapon = 0;
    bool found_weapon = false;
    for (std::size_t id = 0; id < config.weapons.configured.size(); ++id) {
        if (config.weapons.configured[id] &&
            config.weapons.definitions[id].projectile_template_id == bolt_id) {
            weapon = static_cast<std::uint8_t>(id);
            found_weapon = true;
        }
    }
    require(found_weapon);
    const KernelWeaponMechanicsDefinition& staff = config.weapons.definitions[weapon];
    const KernelActionTemplateDefinition* fire = nullptr;
    for (const auto& action : config.action_templates) {
        if (action.definition.action_template_id == staff.fire_action_template_id) {
            fire = &action.definition;
        }
    }
    require(fire != nullptr);
    const int commit_offset = static_cast<int>(fire->commit_offset_ticks);
    const float bolt_range =
        bolt->speed * static_cast<float>(bolt->lifetime_ticks) * kTickSeconds;

    const std::vector<std::uint8_t> scene_bytes =
        read_bytes(runfiles_catalog() / config.static_collision_scene.entry_path);
    Scene scene;
    std::string error;
    require(scene.world.load_static_scene(
        scene_bytes,
        physics::CollisionObjectIdentity{
            0, config.static_collision_scene.collider_id, 0,
            physics::CollisionObjectKind::kTerrain, physics::CollisionLayer::kTerrain},
        &error));

    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 2048;
    kernel_config.max_render_states = 64;
    Arena arena;
    arena.weapon = weapon;
    arena.kernel = Kernel_Create(&kernel_config);
    require(arena.kernel != nullptr);
    require(gs::load_kernel_gameplay_catalog(arena.kernel, config));
    KernelStaticCollisionSceneConfig scene_config{};
    scene_config.struct_size = sizeof(scene_config);
    scene_config.artifact_bytes = scene_bytes.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene_bytes.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(arena.kernel, &scene_config));
    require(Kernel_StartDedicatedServer(arena.kernel, 8094));

    // Open ground in the open combat zone (-x, +z).
    const glm::vec3 open{-50.0f, scene.ground(-50.0f, 50.0f) + 1.0f, 50.0f};
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = config.player.actor_template_id;
    create.actor_template_id = config.player.actor_template_id;
    create.position = to_kernel(open);
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerCreateEntity(arena.kernel, &create, &arena.player));
    require(Kernel_ServerSetEntityActorTemplate(
        arena.kernel, arena.player, config.player.actor_template_id));
    KernelCombatStateDefinition combat = gs::make_player_combat_state(config);
    require(Kernel_ServerSetEntityCombatState(arena.kernel, arena.player, &combat));
    KernelWeaponMechanicsDefinition mechanics = staff;
    require(Kernel_ServerSetEntityWeaponMechanics(arena.kernel, arena.player, &mechanics));
    KernelInventoryContainerId weapons = 0;
    require(Kernel_ServerCreateWeaponContainer(arena.kernel, arena.player, &weapons));
    KernelItemInstanceId item = 0;
    require(Kernel_ServerCreateInventoryItem(arena.kernel, staff_item, 1, weapons, &item));
    arena.idle({1.0f, 0.0f, 0.0f}, 60);
    {
        const KernelServerEntityState me = entity_state(arena.kernel, arena.player);
        require(me.weapon_ids[me.active_weapon_slot] == weapon);
    }

    // The floor: down and ahead, landing a few metres out.
    run_case(arena, scene,
             Case{"floor", open, glm::vec3{1.0f, -0.6f, 0.0f}, 0.99f, 1.0f, 0.0f},
             gingerbread, commit_offset, bolt_range, column_push);

    // A slope: the foot of the narrow pass's east wall, which faces +x near
    // x = -5 and rises at about 33 degrees before it steepens. Found by
    // casting west along the ground from the corridor's open side, so a
    // reshaped scene moves the shooter with it rather than breaking this.
    {
        const float z = -70.0f;
        physics::CollisionHit foot{};
        require(scene.cast({15.0f, scene.ground(15.0f, z) + 1.4f, z},
                           {-1.0f, 0.0f, 0.0f}, 40.0f, &foot));
        const float x = foot.position.x + 7.0f;
        run_case(arena, scene,
                 Case{"slope", {x, scene.ground(x, z) + 1.0f, z}, {-1.0f, 0.0f, 0.0f},
                      0.3f, 0.98f, 1.0f},
                 gingerbread, commit_offset, bolt_range, column_push);
    }

    // A wall: a tree trunk in the forest flank (+x, +z), vertical from the
    // ground up. Found the same way, casting east at chest height.
    {
        const float z = 23.4f;
        physics::CollisionHit trunk{};
        require(scene.cast({60.0f, scene.ground(60.0f, z) + 1.4f, z},
                           {1.0f, 0.0f, 0.0f}, 30.0f, &trunk));
        const float x = trunk.position.x - 7.0f;
        run_case(arena, scene,
                 Case{"wall", {x, scene.ground(x, z) + 1.0f, z}, {1.0f, 0.0f, 0.0f},
                      -0.2f, 0.2f, 3.0f},
                 gingerbread, commit_offset, bolt_range, column_push);
    }

    std::puts("water_jet_staff_test passed");
    return 0;
}
