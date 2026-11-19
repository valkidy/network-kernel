// The Water Jet end to end as a surface strike (docs/PLAYER_WEAPON_REVAMP_PLAN.md
// I3, docs/WATER_JET_PLAN.md): a player armed with the shipped weapon through a
// weapon container fires it through the real input path, the strike puts its
// mark on the face the rules pick, and a gingerbread standing there is shoved
// off that face by the column -- and actually travels, rather than only getting
// a velocity for a tick.
//
// The scene is baked here, not the shipped map, so the faces are known exactly
// and a map change cannot move them: a 200 m floor at y = 0 and a wall whose
// face is the plane x = 20 (normal -x).
//
//   - floor:   aimed down ahead, the first face is the floor: thrown straight up;
//   - wall:    aimed level at the wall: lifted and carried out from it, -x;
//   - reach:   aimed level at nothing within 45 m: the mark goes on the ground
//              under the end of the reach;
//   - sky:     aimed straight up: nothing under the end of the reach within
//              reach of the drop, so the shot is refused and costs nothing.
//
// The push is the column's split impulse, (n.x * h, v, n.z * h) for the face
// normal n, with h and v read from the catalog. A flat push on a grounded
// actor moved it 0.40 m and died the next tick; the lift is what makes it
// carry, which is why the travel is checked and not only the direction.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "game_server/src/gameplay_config.h"
#include "geometry/public/mesh_bake.h"
#include "kernel/public/kernel_api.h"

namespace {

namespace gs = network_example::game_server;

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
constexpr float kWallX = 20.0f;

// The column's push, as the catalog compiles it.
struct Push {
    float horizontal = 0.0f;
    float vertical = 0.0f;

    glm::vec3 delta(const glm::vec3& normal) const {
        return {normal.x * horizontal, vertical, normal.z * horizontal};
    }
};

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

// A floor quad and a closed wall box, baked to the kernel's collision artifact.
std::vector<std::uint8_t> bake_scene() {
    network_example::CanonicalTriangleMesh mesh;
    const auto quad = [&mesh](std::array<float, 3> a, std::array<float, 3> b,
                              std::array<float, 3> c, std::array<float, 3> d) {
        const std::uint32_t base = static_cast<std::uint32_t>(mesh.positions.size());
        mesh.positions.insert(mesh.positions.end(), {a, b, c, d});
        // Wound counter-clockwise seen from the side the face looks out of.
        mesh.triangle_indices.insert(
            mesh.triangle_indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    // Floor, facing +y.
    quad({-100.0f, 0.0f, -100.0f}, {-100.0f, 0.0f, 100.0f},
         {100.0f, 0.0f, 100.0f}, {100.0f, 0.0f, -100.0f});
    // The wall: x 20..22, y 0..10, z -10..10.
    constexpr float x0 = kWallX, x1 = kWallX + 2.0f, y1 = 10.0f, z0 = -10.0f, z1 = 10.0f;
    quad({x0, 0.0f, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, 0.0f, z0});  // -x, the face
    quad({x1, 0.0f, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, 0.0f, z1});  // +x
    quad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0});      // +y
    quad({x0, 0.0f, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, 0.0f, z0});  // -z
    quad({x1, 0.0f, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, 0.0f, z1});  // +z
    mesh.bounds.min = {-100.0f, 0.0f, -100.0f};
    mesh.bounds.max = {100.0f, y1, 100.0f};
    const network_example::MeshBakeResult baked = network_example::bake_mesh_artifacts(
        mesh, network_example::default_mesh_bake_config(), 1u, 1u);
    if (!baked) std::fprintf(stderr, "bake: %s\n", baked.error.c_str());
    require(static_cast<bool>(baked));
    return baked.artifacts.jolt;
}

struct Arena {
    KernelHandle* kernel = nullptr;
    std::uint8_t weapon = 0;
    std::uint32_t player = 0;
    std::uint32_t next_seq = 1;
    std::uint32_t next_action = 1;
    std::uint16_t mark_template = 0;

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

    std::uint16_t ammo() const {
        const KernelServerEntityState me = entity_state(kernel, player);
        return me.ammo[me.active_weapon_slot];
    }

    // One press, then waiting out the cast: the mark, as it was on the first
    // tick it existed, or nothing if the strike was refused.
    std::optional<glm::vec3> fire(const glm::vec3& aim, int commit_offset) {
        require(projectiles().empty());
        const std::uint32_t action = 9200u + next_action++;
        std::optional<glm::vec3> mark;
        for (int index = 0; index <= commit_offset + 2 && !mark.has_value(); ++index) {
            send(aim, action, index == 0, false);
            for (const KernelServerEntityState& state : projectiles()) {
                // The mark is still: speed 0.
                if (glm::length(to_glm(state.velocity)) < 0.01f) {
                    mark = to_glm(state.position);
                }
            }
        }
        return mark;
    }

    void wait_for_no_projectiles(const glm::vec3& aim) {
        for (int tick = 0; tick < 150 && !projectiles().empty(); ++tick) idle(aim, 1);
        require(projectiles().empty());
    }

    // The shipped magazine is 4 and every case fires twice: each case starts
    // on a full one, its reload waited out first.
    void reload(const glm::vec3& aim, int reload_ticks) {
        const std::uint32_t action = 9200u + next_action++;
        KernelPlayerInput input{};
        input.input_seq = next_seq++;
        input.aim_dir = to_kernel(glm::normalize(aim));
        input.selected_weapon = weapon;
        input.action_intent = KernelActionIntent{action, KernelActionBinding_Reload, 0u, 0u};
        input.action_input = KernelActionInput{action, 0u, 0u, 0u};
        require(Kernel_ServerSubmitEntityInput(kernel, player, &input));
        Kernel_Update(kernel, kTickSeconds);
        idle(aim, reload_ticks + 5);
    }

    void stand(const glm::vec3& at) {
        const KernelServerEntityState me = entity_state(kernel, player);
        const KernelVec3 position = to_kernel(at);
        require(Kernel_ServerSetEntityTransform(kernel, player, &position, &me.rotation));
    }
};

struct Case {
    const char* name;
    glm::vec3 shooter;
    glm::vec3 aim;
    // Where the mark has to be, and the face it is on.
    glm::vec3 mark;
    float mark_tolerance;
    glm::vec3 normal;
    // How far out it must at least be carried, whatever the tuning.
    float min_carry;
};

// Fires the case and holds the mark to the face, then fires again with a
// gingerbread standing on the spot and holds its push to the face's normal.
void run_case(Arena& arena, const Case& c, std::uint32_t gingerbread,
              int commit_offset, int reload_ticks, const Push& column_push) {
    arena.stand(c.shooter);
    arena.idle(c.aim, 30);
    const std::uint16_t full = arena.ammo();
    arena.reload(c.aim, reload_ticks);
    require(arena.ammo() >= 2u && arena.ammo() >= full);

    // Shot one: where does the mark go?
    {
        const KernelServerEntityState me = entity_state(arena.kernel, arena.player);
        std::fprintf(stderr, "water_jet[%s]: ammo %u reserve %u reloading %u at (%.2f %.2f %.2f)\n",
                     c.name, me.ammo[me.active_weapon_slot],
                     me.reserve_magazines[me.active_weapon_slot], me.is_reloading,
                     me.position.x, me.position.y, me.position.z);
    }
    const std::optional<glm::vec3> mark = arena.fire(c.aim, commit_offset);
    require(mark.has_value());
    std::fprintf(stderr, "water_jet[%s]: mark at (%.2f %.2f %.2f), expected (%.2f %.2f %.2f)\n",
                 c.name, mark->x, mark->y, mark->z, c.mark.x, c.mark.y, c.mark.z);
    require(glm::length(*mark - c.mark) < c.mark_tolerance);
    arena.wait_for_no_projectiles(c.aim);

    // Shot two, with a gingerbread standing just off the face, on the floor.
    glm::vec3 out{c.normal.x, 0.0f, c.normal.z};
    out = glm::length(out) > 0.1f ? glm::normalize(out) : glm::vec3{0.0f};
    glm::vec3 stand = *mark + out * 0.8f;
    stand.y = 0.0f;
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

    const std::optional<glm::vec3> again = arena.fire(c.aim, commit_offset);
    require(again.has_value());
    // The strike passes through actors: the same mark, gingerbread or not.
    require(glm::length(*again - *mark) < 0.05f);
    // The first tick the gingerbread moves fast is the push; gravity has had
    // at most a tick at it. The mark stands 30 ticks before the column rises.
    glm::vec3 push{0.0f};
    for (int tick = 0; tick < 90 && glm::length(push) < 5.0f; ++tick) {
        arena.idle(c.aim, 1);
        push = to_glm(entity_state(arena.kernel, target).velocity);
    }
    const glm::vec3 expected = column_push.delta(c.normal);
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
    std::uint32_t jet_item = 0;
    for (const auto& item : config.item_templates) {
        if (item.name == "stateful_weapon_water_jet") jet_item = item.definition.item_template_id;
    }
    std::uint32_t gingerbread = 0;
    for (const auto& entity : config.entity_templates) {
        if (entity.name == "gingerbread") gingerbread = entity.actor_template_id;
    }
    require(jet_item != 0u && gingerbread != 0u);
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
    require(column_push.vertical > 0.0f && column_push.horizontal > 0.0f);
    std::uint8_t weapon = 0;
    bool found_weapon = false;
    for (std::size_t id = 0; id < config.weapons.configured.size(); ++id) {
        if (config.weapons.configured[id] && config.weapons.names[id] == "Water Jet") {
            weapon = static_cast<std::uint8_t>(id);
            found_weapon = true;
        }
    }
    require(found_weapon);
    const KernelWeaponMechanicsDefinition& jet = config.weapons.definitions[weapon];
    require(jet.fire_mode == KernelWeaponFireMode_TargetedStrikeSurface);
    const float reach = jet.max_range;
    require(reach > 10.0f);
    const KernelActionTemplateDefinition* fire = nullptr;
    for (const auto& action : config.action_templates) {
        if (action.definition.action_template_id == jet.fire_action_template_id) {
            fire = &action.definition;
        }
    }
    require(fire != nullptr);
    const int commit_offset = static_cast<int>(fire->commit_offset_ticks);
    int reload_ticks = 0;
    for (const auto& action : config.action_templates) {
        if (action.definition.action_template_id == jet.reload_action_template_id) {
            reload_ticks = static_cast<int>(action.definition.commit_offset_ticks);
        }
    }
    require(reload_ticks > 0);

    const std::vector<std::uint8_t> scene = bake_scene();
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
    scene_config.artifact_bytes = scene.data();
    scene_config.artifact_size = static_cast<std::uint32_t>(scene.size());
    scene_config.scene_id = config.static_collision_scene.scene_id;
    scene_config.collider_id = config.static_collision_scene.collider_id;
    scene_config.collision_layer = config.static_collision_scene.collision_layer;
    require(Kernel_SetStaticCollisionScene(arena.kernel, &scene_config));
    require(Kernel_StartDedicatedServer(arena.kernel, 8094));

    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = gs::kEntityTypeActor;
    create.actor_type = gs::kActorTypePlayer;
    create.entity_template_id = config.player.actor_template_id;
    create.actor_template_id = config.player.actor_template_id;
    create.position = KernelVec3{-60.0f, 0.5f, -60.0f};
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    require(Kernel_ServerCreateEntity(arena.kernel, &create, &arena.player));
    require(Kernel_ServerSetEntityActorTemplate(
        arena.kernel, arena.player, config.player.actor_template_id));
    KernelCombatStateDefinition combat = gs::make_player_combat_state(config);
    require(Kernel_ServerSetEntityCombatState(arena.kernel, arena.player, &combat));
    KernelWeaponMechanicsDefinition mechanics = jet;
    require(Kernel_ServerSetEntityWeaponMechanics(arena.kernel, arena.player, &mechanics));
    KernelInventoryContainerId weapons = 0;
    require(Kernel_ServerCreateWeaponContainer(arena.kernel, arena.player, &weapons));
    KernelItemInstanceId item = 0;
    require(Kernel_ServerCreateInventoryItem(arena.kernel, jet_item, 1, weapons, &item));
    arena.idle({1.0f, 0.0f, 0.0f}, 60);
    {
        const KernelServerEntityState me = entity_state(arena.kernel, arena.player);
        require(me.weapon_ids[me.active_weapon_slot] == weapon);
    }
    // Where the strike's rays start: the player's feet, settled, plus 1 m.
    const float eye = entity_state(arena.kernel, arena.player).position.y + 1.0f;
    std::fprintf(stderr, "water_jet: rays leave at y %.2f, reach %.1f m\n", eye, reach);

    // The floor: down and ahead at 45 degrees, landing `eye` metres out.
    {
        const glm::vec3 shooter{-60.0f, 0.0f, -60.0f};
        run_case(arena,
                 Case{"floor", shooter, glm::vec3{1.0f, -1.0f, 0.0f},
                      {shooter.x + eye, 0.0f, shooter.z}, 0.3f, {0.0f, 1.0f, 0.0f}, 0.0f},
                 gingerbread, commit_offset, reload_ticks, column_push);
    }

    // The wall, 8 m ahead and level: the face x = 20 at eye height, out -x.
    {
        const glm::vec3 shooter{kWallX - 8.0f, 0.0f, 0.0f};
        run_case(arena,
                 Case{"wall", shooter, glm::vec3{1.0f, 0.0f, 0.0f},
                      {kWallX, eye, 0.0f}, 0.3f, {-1.0f, 0.0f, 0.0f}, 3.0f},
                 gingerbread, commit_offset, reload_ticks, column_push);
    }

    // Level at nothing within reach: the ground under the end of it.
    {
        const glm::vec3 shooter{-60.0f, 0.0f, 40.0f};
        run_case(arena,
                 Case{"reach", shooter, glm::vec3{0.0f, 0.0f, 1.0f},
                      {shooter.x, 0.0f, shooter.z + reach}, 0.3f, {0.0f, 1.0f, 0.0f}, 0.0f},
                 gingerbread, commit_offset, reload_ticks, column_push);
    }

    // Straight up: the end of the reach is `reach` above the eye, and the
    // drop, as long as the reach, stops `eye` short of the floor. Refused,
    // and refused for free.
    {
        arena.stand({-60.0f, 0.0f, -60.0f});
        arena.idle({0.0f, 1.0f, 0.0f}, 30);
        arena.reload({0.0f, 1.0f, 0.0f}, reload_ticks);
        const std::uint16_t before = arena.ammo();
        require(before > 0u);
        require(!arena.fire({0.0f, 1.0f, 0.0f}, commit_offset).has_value());
        arena.idle({0.0f, 1.0f, 0.0f}, 40);
        require(arena.projectiles().empty());
        require(arena.ammo() == before);
        std::fprintf(stderr, "water_jet[sky]: refused, ammo %u kept\n", before);
    }

    std::puts("water_jet_staff_test passed");
    return 0;
}
