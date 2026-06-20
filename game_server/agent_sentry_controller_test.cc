#include "game_server/agent_sentry_controller.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "game_server/gameplay_config.h"

namespace {

KernelConfig server_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 30;
    config.max_events = 64;
    config.max_render_states = 64;
    return config;
}

KernelColliderTemplateDefinition collider_template() {
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

KernelColliderTemplateDefinition vision_collider_template() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = 2;
    collider.shape_type = KernelColliderShapeType_Cone;
    collider.shape_params = KernelVec4{10.0f, 90.0f, 0.0f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Vision;
    collider.layer_mask = KERNEL_COLLISION_LAYER_AGENT_VISION;
    return collider;
}

void load_catalog(KernelHandle* kernel) {
    const std::array<KernelColliderTemplateDefinition, 2> colliders = {
        collider_template(),
        vision_collider_template(),
    };
    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 1;
    catalog.collider_templates = colliders.data();
    catalog.collider_template_count = static_cast<std::uint32_t>(colliders.size());
    assert(Kernel_LoadGameplayCatalog(kernel, &catalog));
}

std::uint32_t create_entity(
    KernelHandle* kernel,
    std::uint16_t actor_type,
    const KernelVec3& position) {
    KernelServerEntityCreateInfo create_info{};
    create_info.struct_size = sizeof(create_info);
    create_info.entity_type = network_example::game_server::kEntityTypeActor;
    create_info.actor_type = actor_type;
    create_info.position = position;
    create_info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    assert(Kernel_ServerCreateEntity(kernel, &create_info, &net_id));
    assert(net_id != 0);
    return net_id;
}

void set_combat(KernelHandle* kernel, std::uint32_t net_id) {
    KernelCombatStateDefinition combat{};
    combat.struct_size = sizeof(combat);
    combat.hp = 100;
    combat.max_hp = 100;
    combat.active_weapon_id = network_example::game_server::kAgentSpammerWeaponId;
    combat.collider_template_id = 1;
    combat.hitbox_center = KernelVec3{0.0f, 0.8f, 0.0f};
    combat.hitbox_half_extents = KernelVec3{0.4f, 0.8f, 0.4f};
    combat.ammo[network_example::game_server::kAgentSpammerWeaponId] =
        network_example::game_server::kAgentSpammerMagazine;
    assert(Kernel_ServerSetEntityCombatState(kernel, net_id, &combat));
}

void set_vision(
    KernelHandle* kernel,
    std::uint32_t net_id,
    std::uint8_t camp,
    std::uint32_t vision_collider_template_id) {
    KernelAgentVisionConfig vision{};
    vision.struct_size = sizeof(vision);
    vision.camp = camp;
    vision.vision_collider_template_id = vision_collider_template_id;
    vision.max_visible_hostiles = KERNEL_MAX_VISIBLE_HOSTILES;
    vision.max_visible_allies = KERNEL_MAX_VISIBLE_ALLIES;
    assert(Kernel_ServerSetEntityVisionConfig(kernel, net_id, &vision));
}

void run_frame(
    KernelHandle* kernel,
    const network_example::game_server::AgentSentryController& controller,
    std::vector<network_example::game_server::Enemy>* enemies) {
    controller.tick(kernel, enemies, 1.0f / 30.0f);
    Kernel_Update(kernel, 1.0f / 30.0f);
}

KernelQuat query_rotation(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    assert(Kernel_ServerGetEntityState(kernel, net_id, &state));
    assert(state.valid != 0u);
    return state.rotation;
}

bool almost_equal(float lhs, float rhs, float tolerance = 0.001f) {
    return std::fabs(lhs - rhs) <= tolerance;
}

}  // namespace

int main() {
    KernelConfig config = server_config();
    KernelHandle* kernel = Kernel_Create(&config);
    assert(kernel != nullptr);
    assert(Kernel_StartDedicatedServer(kernel, 7777));
    load_catalog(kernel);

    const std::uint32_t enemy_net_id =
        create_entity(kernel, network_example::game_server::kActorTypeAgent, {0.0f, 0.0f, 0.0f});
    const std::uint32_t player_net_id =
        create_entity(kernel, network_example::game_server::kActorTypePlayer, {5.0f, 0.0f, 0.0f});
    set_combat(kernel, enemy_net_id);
    set_vision(kernel, enemy_net_id, KernelAgentCamp_EnemySide, 2);
    set_vision(kernel, player_net_id, KernelAgentCamp_PlayerSide, 0);
    Kernel_Update(kernel, 1.0f / 30.0f);

    network_example::game_server::Enemy enemy;
    enemy.net_id = enemy_net_id;
    enemy.position = KernelVec3{0.0f, 0.0f, 0.0f};
    enemy.ammo = network_example::game_server::kAgentSpammerMagazine;
    std::vector<network_example::game_server::Enemy> enemies{enemy};

    network_example::game_server::AgentSentryConfig sentry_config;
    sentry_config.fire_interval_seconds = 0.01f;
    sentry_config.alert_seconds = 100.0f;
    sentry_config.alert_rotation_interval_ticks = 2;
    sentry_config.alert_rotation_degrees = 15.0f;
    network_example::game_server::AgentSentryController controller(sentry_config);

    run_frame(kernel, controller, &enemies);
    assert(enemies[0].sentry.state == network_example::game_server::AgentSentryState::kAlert);
    assert(enemies[0].ammo == network_example::game_server::kAgentSpammerMagazine);
    KernelQuat rotation = query_rotation(kernel, enemy_net_id);
    assert(almost_equal(rotation.x, 0.0f));
    assert(almost_equal(rotation.y, 0.0f));
    assert(almost_equal(rotation.z, 0.0f));
    assert(almost_equal(rotation.w, 1.0f));

    run_frame(kernel, controller, &enemies);
    rotation = query_rotation(kernel, enemy_net_id);
    const float half_alert_rotation_radians =
        (15.0f * 0.5f) * 3.14159265358979323846f / 180.0f;
    assert(almost_equal(
        std::fabs(rotation.y),
        std::sin(half_alert_rotation_radians)));
    assert(almost_equal(rotation.w, std::cos(half_alert_rotation_radians)));

    sentry_config.alert_seconds = 0.01f;
    network_example::game_server::AgentSentryController attack_controller(sentry_config);

    run_frame(kernel, attack_controller, &enemies);
    assert(enemies[0].sentry.state == network_example::game_server::AgentSentryState::kAttack);
    KernelVec3 side_position{5.0f, 0.0f, 2.0f};
    KernelQuat identity{0.0f, 0.0f, 0.0f, 1.0f};
    assert(Kernel_ServerSetEntityTransform(kernel, player_net_id, &side_position, &identity));
    Kernel_Update(kernel, 1.0f / 30.0f);
    attack_controller.tick(kernel, &enemies, 1.0f / 30.0f);
    assert(enemies[0].ammo < network_example::game_server::kAgentSpammerMagazine);
    rotation = query_rotation(kernel, enemy_net_id);
    const float expected_yaw = std::atan2(2.0f, 5.0f);
    assert(almost_equal(rotation.y, -std::sin(expected_yaw * 0.5f)));
    assert(almost_equal(rotation.w, std::cos(expected_yaw * 0.5f)));

    KernelVec3 out_of_range{100.0f, 0.0f, 0.0f};
    assert(Kernel_ServerSetEntityTransform(kernel, player_net_id, &out_of_range, &identity));
    Kernel_Update(kernel, 1.0f / 30.0f);
    attack_controller.tick(kernel, &enemies, 1.0f / 30.0f);
    assert(enemies[0].sentry.state == network_example::game_server::AgentSentryState::kAlert);

    for (int frame = 0; frame < 151; ++frame) {
        run_frame(kernel, attack_controller, &enemies);
    }
    assert(enemies[0].sentry.state == network_example::game_server::AgentSentryState::kIdle);
    assert(enemies[0].velocity.x == 0.0f);
    assert(enemies[0].velocity.y == 0.0f);
    assert(enemies[0].velocity.z == 0.0f);

    Kernel_Destroy(kernel);
    return 0;
}
