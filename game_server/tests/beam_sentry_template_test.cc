#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "game_server/src/gameplay_config.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

}  // namespace

#define require(condition) \
    require_impl(static_cast<bool>(condition), #condition, __LINE__)

namespace gs = network_example::game_server;

int main() {
    const char* srcdir = std::getenv("TEST_SRCDIR");
    const char* workspace = std::getenv("TEST_WORKSPACE");
    require(srcdir != nullptr && workspace != nullptr);
    const std::filesystem::path catalog =
        std::filesystem::path(srcdir) / workspace / "game_server" /
        "gameplay_catalog" / "gameplay_catalog.yaml";

    const gs::GameServerGameplayConfig config =
        gs::load_gameplay_config_from_catalog_file(catalog.string());

    const gs::ActorTemplateConfig* actor = nullptr;
    for (const gs::ActorTemplateConfig& candidate : config.actor_templates) {
        if (candidate.name == "beam_sentry") {
            actor = &candidate;
        }
    }
    require(actor != nullptr);
    std::printf("actor id=%u weapon_slot_count=%u slot0=%u sentry.weapon_id=%u\n",
                actor->actor_template_id,
                actor->weapon_slot_count,
                actor->weapon_ids[0],
                actor->sentry.weapon_id);
    require(actor->weapon_slot_count == 1);
    require(actor->weapon_ids[0] == 8u);
    require(actor->sentry.weapon_id == 8u);
    require(actor->ai_controller_type == KernelAiControllerType_Sentry);

    require(config.weapons.configured[8]);
    const KernelWeaponMechanicsDefinition& weapon = config.weapons.definitions[8];
    std::printf("weapon fire_mode=%u magazine=%u reserve=%u proj=%u fire_action=%u reload_action=%u\n",
                weapon.fire_mode,
                weapon.magazine_size,
                weapon.reserve_magazines,
                weapon.projectile_template_id,
                weapon.fire_action_template_id,
                weapon.reload_action_template_id);
    require(weapon.fire_mode == KernelWeaponFireMode_Projectile);
    require(weapon.fire_action_template_id == 4104u);
    require(weapon.projectile_template_id == 9u);

    const KernelProjectileMechanicsDefinition* mech = nullptr;
    for (const gs::ProjectileTemplateConfig& projectile :
         config.projectile_templates) {
        if (projectile.definition.projectile_template_id == 9u) {
            mech = &projectile.definition.mechanics;
            std::printf("projectile bound to weapon_id=%u\n",
                        projectile.definition.weapon_id);
            require(projectile.definition.weapon_id == 8u);
        }
    }
    require(mech != nullptr);
    std::printf("beam len=%.2f radius=%.2f dpt=%u lifetime=%u mask=0x%x top_mask=0x%x type=%u sync=%u\n",
                mech->beam.length,
                mech->beam.radius,
                mech->beam.damage_per_tick,
                mech->beam.lifetime_ticks,
                mech->beam.collision_mask,
                mech->collision_mask,
                mech->projectile_type,
                mech->sync_mode);
    require(mech->projectile_type == KernelProjectileType_Beam);
    require(mech->beam.collision_mask ==
           (KERNEL_COLLISION_LAYER_PLAYER_SIDE | KERNEL_COLLISION_LAYER_TERRAIN |
            KERNEL_COLLISION_LAYER_STATIC_OBSTACLE));
    require((mech->beam.collision_mask & KERNEL_COLLISION_LAYER_HOSTILE_SIDE) == 0u);
    require(mech->beam.damage_per_tick == 2u);
    require(mech->beam.lifetime_ticks == 2u);

    // The collider is the authority on reach; confirm it beat the YAML value
    // through the kernel conversion, and that it out-ranges the vision cone.
    const KernelColliderTemplateDefinition* vision = nullptr;
    for (const gs::ColliderTemplateConfig& collider : config.colliders.templates) {
        if (collider.name == "sentry_grunt_vision_cone") {
            vision = &collider.definition;
        }
    }
    require(vision != nullptr);
    std::printf("vision range=%.2f beam length=%.2f\n",
                vision->shape_params.x,
                mech->beam.length);
    require(mech->beam.length > vision->shape_params.x);

    std::printf("OK\n");
    return 0;
}
