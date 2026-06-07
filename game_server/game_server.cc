#include "game_server/game_server.h"

#include <cstring>
#include <utility>
#include <vector>

namespace network_example::game_server {
namespace {

std::uint8_t projectile_sync_mode_for_weapon(
    const KernelWeaponMechanicsDefinition& weapon) {
    if (weapon.projectile.motion_model == KernelProjectileMotionModel_Homing &&
        weapon.projectile.homing.struct_size >= sizeof(KernelHomingMechanicsDefinition)) {
        return weapon.projectile.homing.sync_mode;
    }
    return KernelProjectileSyncMode_HybridDeterministicThenSnapshot;
}

void load_kernel_catalog(
    KernelHandle* kernel,
    const GameServerGameplayConfig& config) {
    if (kernel == nullptr) {
        return;
    }

    std::vector<KernelProjectileTemplateDefinition> projectile_templates;
    for (const KernelWeaponMechanicsDefinition& weapon : config.weapons.definitions) {
        if (weapon.fire_mode != KernelWeaponFireMode_Projectile) {
            continue;
        }
        KernelProjectileTemplateDefinition projectile_template{};
        projectile_template.struct_size = sizeof(projectile_template);
        projectile_template.projectile_template_id = weapon.weapon_id;
        projectile_template.weapon_id = weapon.weapon_id;
        projectile_template.motion_model = weapon.projectile.motion_model;
        projectile_template.sync_mode = projectile_sync_mode_for_weapon(weapon);
        projectile_template.hit_response = weapon.projectile.hit_response;
        projectile_template.damage_shape = weapon.projectile.damage_shape;
        projectile_template.damage = weapon.damage;
        projectile_template.speed = weapon.projectile.speed;
        projectile_template.lifetime_seconds = weapon.projectile.lifetime_seconds;
        projectile_template.explosion_radius = weapon.projectile.explosion_radius;
        projectile_template.gravity = weapon.projectile.gravity;
        projectile_template.collider_template_id = 3;
        projectile_template.explosion_template_id =
            weapon.projectile.explosion_radius > 0.0f ? 4u : 0u;
        projectile_template.collision_mask = weapon.projectile.collision_mask;
        projectile_template.max_hit_count = weapon.projectile.max_hit_count;
        projectile_templates.push_back(projectile_template);
    }

    const KernelColliderTemplateDefinition collider_templates[] = {
        KernelColliderTemplateDefinition{
            sizeof(KernelColliderTemplateDefinition),
            1,
            KernelColliderShapeType_Aabb,
            0,
            0,
            KernelVec3{0.0f, 0.0f, 0.0f},
            config.player.hitbox_half_extents,
            0.0f,
            KernelColliderPurpose_Hit,
            KERNEL_COLLISION_LAYER_PLAYER,
        },
        KernelColliderTemplateDefinition{
            sizeof(KernelColliderTemplateDefinition),
            2,
            KernelColliderShapeType_Aabb,
            0,
            0,
            KernelVec3{0.0f, 0.0f, 0.0f},
            config.enemy.hitbox_half_extents,
            0.0f,
            KernelColliderPurpose_Hit,
            KERNEL_COLLISION_LAYER_ENEMY,
        },
        KernelColliderTemplateDefinition{
            sizeof(KernelColliderTemplateDefinition),
            3,
            KernelColliderShapeType_Aabb,
            0,
            0,
            KernelVec3{0.0f, 0.0f, 0.0f},
            KernelVec3{0.1f, 0.1f, 0.1f},
            0.0f,
            KernelColliderPurpose_Damage,
            KERNEL_COLLISION_LAYER_PROJECTILE,
        },
        KernelColliderTemplateDefinition{
            sizeof(KernelColliderTemplateDefinition),
            4,
            KernelColliderShapeType_Sphere,
            0,
            0,
            KernelVec3{0.0f, 0.0f, 0.0f},
            KernelVec3{1.0f, 1.0f, 1.0f},
            1.0f,
            KernelColliderPurpose_Damage,
            KERNEL_COLLISION_LAYER_AREA_EFFECT,
        },
    };
    const KernelColliderBindingDefinition collider_bindings[] = {
        KernelColliderBindingDefinition{
            sizeof(KernelColliderBindingDefinition),
            config.player.entity_type,
            0,
            1,
            config.player.hitbox_center,
            KernelQuat{0.0f, 0.0f, 0.0f, 1.0f},
        },
        KernelColliderBindingDefinition{
            sizeof(KernelColliderBindingDefinition),
            config.enemy.entity_type,
            0,
            2,
            config.enemy.hitbox_center,
            KernelQuat{0.0f, 0.0f, 0.0f, 1.0f},
        },
    };

    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = config.weapons.catalog_version;
    catalog.catalog_hash = config.weapons.catalog_hash;
    catalog.projectile_templates = projectile_templates.data();
    catalog.projectile_template_count =
        static_cast<std::uint32_t>(projectile_templates.size());
    catalog.collider_templates = collider_templates;
    catalog.collider_template_count =
        static_cast<std::uint32_t>(std::size(collider_templates));
    catalog.collider_bindings = collider_bindings;
    catalog.collider_binding_count =
        static_cast<std::uint32_t>(std::size(collider_bindings));
    Kernel_LoadGameplayCatalog(kernel, &catalog);
}

}  // namespace

GameServer::GameServer(KernelHandle* kernel, GameServerGameplayConfig config)
    : kernel_(kernel),
      config_(std::move(config)),
      enemy_manager_(kernel, config_) {
    load_kernel_catalog(kernel_, config_);
}

void GameServer::handle_event(const KernelEvent& event) {
    if (event.type == KernelEventType_PlayerJoined && event.net_id != 0) {
        configure_player(event.net_id);
    }
    enemy_manager_.handle_event(event);
}

void GameServer::tick(float delta_seconds) {
    enemy_manager_.tick(delta_seconds);
}

EnemyManager& GameServer::enemy_manager() {
    return enemy_manager_;
}

const EnemyManager& GameServer::enemy_manager() const {
    return enemy_manager_;
}

bool GameServer::query_weapon_template(
    std::uint8_t weapon_id,
    GameServerWeaponTemplateInfo* out_info) const {
    if (out_info == nullptr ||
        out_info->struct_size < sizeof(GameServerWeaponTemplateInfo) ||
        weapon_id >= kWeaponCount) {
        return false;
    }
    const KernelWeaponMechanicsDefinition& mechanics =
        config_.weapons.definitions[weapon_id];
    if (mechanics.struct_size < sizeof(KernelWeaponMechanicsDefinition)) {
        return false;
    }
    std::memset(out_info, 0, sizeof(GameServerWeaponTemplateInfo));
    out_info->struct_size = sizeof(GameServerWeaponTemplateInfo);
    out_info->weapon_id = weapon_id;
    out_info->fire_mode = mechanics.fire_mode;
    const std::string& name = config_.weapons.names[weapon_id];
    std::strncpy(out_info->name, name.c_str(), sizeof(out_info->name) - 1);
    out_info->mechanics = mechanics;
    out_info->valid = true;
    return true;
}

void GameServer::configure_player(std::uint32_t net_id) const {
    if (kernel_ == nullptr) {
        return;
    }
    KernelCombatStateDefinition combat_state = make_player_combat_state(config_);
    if (!Kernel_ServerSetEntityCombatState(kernel_, net_id, &combat_state)) {
        return;
    }
    for (const KernelWeaponMechanicsDefinition& weapon : config_.weapons.definitions) {
        Kernel_ServerSetEntityWeaponMechanics(kernel_, net_id, &weapon);
    }
}

}  // namespace network_example::game_server
