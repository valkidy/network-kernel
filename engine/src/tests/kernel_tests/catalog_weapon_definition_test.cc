// ABI 104: a gameplay catalog carries its weapons, and the kernel installs them
// in every mode. A pure client never receives the per-entity mechanics a server
// sets, so this table is the only place it can read a lock-on weapon's cone
// from. A weapon whose cone does not resolve rejects the whole catalog, the way
// a server refuses the same weapon at configuration time.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "kernel/public/kernel_api.h"

namespace {

constexpr std::uint32_t kConeTemplate = 30u;
constexpr std::uint32_t kShotCollider = 31u;
constexpr std::uint32_t kShotTemplate = 40u;
constexpr std::uint32_t kFireAction = 5001u;
constexpr std::uint32_t kReloadAction = 5002u;
constexpr std::uint8_t kLockWeapon = 21u;

void require_impl(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d\n", line);
        std::abort();
    }
}

#define require(condition) require_impl((condition), __LINE__)

KernelColliderTemplateDefinition cone(std::uint8_t shape_type) {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = kConeTemplate;
    collider.shape_type = shape_type;
    // range, fov_degrees
    collider.shape_params = KernelVec4{18.0f, 50.0f, 0.0f, 0.0f};
    collider.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    collider.purpose_flags = KernelColliderPurpose_Damage;
    return collider;
}

KernelColliderTemplateDefinition shot_collider() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = kShotCollider;
    collider.shape_type = KernelColliderShapeType_Sphere;
    collider.shape_params = KernelVec4{0.25f, 0.0f, 0.0f, 0.0f};
    collider.layer_mask = KERNEL_COLLISION_LAYER_PROJECTILE;
    collider.purpose_flags = KernelColliderPurpose_Damage;
    return collider;
}

KernelProjectileTemplateDefinition shot() {
    KernelProjectileTemplateDefinition shot{};
    shot.struct_size = sizeof(shot);
    shot.projectile_template_id = kShotTemplate;
    shot.mechanics.struct_size = sizeof(KernelProjectileMechanicsDefinition);
    shot.mechanics.projectile_type = KernelProjectileType_Standard;
    shot.mechanics.motion_model = KernelProjectileMotionModel_Linear;
    shot.mechanics.sync_mode = KernelProjectileSyncMode_ServerSnapshotOnly;
    shot.mechanics.hit_response = KernelProjectileHitResponse_Destroy;
    shot.mechanics.damage_shape = KernelProjectileDamageShape_DirectHit;
    shot.mechanics.damage = 12;
    shot.mechanics.speed = 10.0f;
    shot.mechanics.lifetime_ticks = 3;
    shot.mechanics.collider_template_id = kShotCollider;
    shot.mechanics.collision_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
    shot.mechanics.max_hit_count = 4;
    return shot;
}

KernelWeaponMechanicsDefinition lock_weapon() {
    KernelWeaponMechanicsDefinition weapon{};
    weapon.struct_size = sizeof(weapon);
    weapon.weapon_id = kLockWeapon;
    weapon.fire_mode = KernelWeaponFireMode_LockInstant;
    weapon.magazine_size = 24;
    weapon.reserve_magazines = 2;
    weapon.damage = 12;
    weapon.pellet_count = 1;
    weapon.projectile_template_id = kShotTemplate;
    weapon.fire_action_template_id = kFireAction;
    weapon.reload_action_template_id = kReloadAction;
    weapon.collision_mask = KERNEL_COLLISION_LAYER_HOSTILE_SIDE;
    weapon.melee_collider_template_id = kConeTemplate;
    return weapon;
}

bool load(
    KernelHandle* kernel,
    std::uint8_t cone_shape,
    const KernelWeaponMechanicsDefinition& weapon) {
    const std::array<KernelColliderTemplateDefinition, 2> colliders = {
        cone(cone_shape),
        shot_collider(),
    };
    const std::array<KernelProjectileTemplateDefinition, 1> projectiles = {shot()};
    KernelActionTemplateDefinition fire{};
    fire.struct_size = sizeof(fire);
    fire.action_template_id = kFireAction;
    fire.trigger_mode = KernelActionTriggerMode_Hold;
    fire.ammo_cost_per_commit = 4u;
    fire.commit_interval_ticks = 6u;
    fire.hold_input_timeout_ticks = 6u;
    KernelActionTemplateDefinition reload{};
    reload.struct_size = sizeof(reload);
    reload.action_template_id = kReloadAction;
    reload.trigger_mode = KernelActionTriggerMode_Press;
    reload.commit_offset_ticks = 30u;
    reload.max_commit_count = 1u;
    const std::array<KernelActionTemplateDefinition, 2> actions = {fire, reload};
    const std::array<KernelWeaponMechanicsDefinition, 1> weapons = {weapon};

    KernelGameplayCatalogDefinition catalog{};
    catalog.struct_size = sizeof(catalog);
    catalog.catalog_version = 1;
    catalog.catalog_hash = 0x10c4ull;
    catalog.collider_templates = colliders.data();
    catalog.collider_template_count = static_cast<std::uint32_t>(colliders.size());
    catalog.projectile_templates = projectiles.data();
    catalog.projectile_template_count =
        static_cast<std::uint32_t>(projectiles.size());
    catalog.action_templates = actions.data();
    catalog.action_template_count = static_cast<std::uint32_t>(actions.size());
    catalog.weapon_definitions = weapons.data();
    catalog.weapon_definition_count = static_cast<std::uint32_t>(weapons.size());
    return Kernel_LoadGameplayCatalog(kernel, &catalog, nullptr);
}

KernelHandle* create_client() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    KernelHandle* kernel = Kernel_Create(&config);
    require(kernel != nullptr);
    return kernel;
}

void a_pure_client_reads_a_catalog_weapon_back() {
    KernelHandle* kernel = create_client();
    require(load(kernel, KernelColliderShapeType_Cone, lock_weapon()));

    KernelWeaponMechanicsDefinition read{};
    read.struct_size = sizeof(read);
    require(Kernel_GetCatalogWeaponDefinition(kernel, kLockWeapon, &read));
    require(read.weapon_id == kLockWeapon);
    require(read.fire_mode == KernelWeaponFireMode_LockInstant);
    require(read.melee_collider_template_id == kConeTemplate);
    require(read.projectile_template_id == kShotTemplate);
    require(read.fire_action_template_id == kFireAction);
    require(read.magazine_size == 24);

    // A weapon the catalog does not name, and a caller that hands a struct
    // too small to hold the answer, both get false.
    KernelWeaponMechanicsDefinition missing{};
    missing.struct_size = sizeof(missing);
    require(!Kernel_GetCatalogWeaponDefinition(kernel, kLockWeapon + 1u, &missing));
    KernelWeaponMechanicsDefinition too_small{};
    too_small.struct_size = 4u;
    require(!Kernel_GetCatalogWeaponDefinition(kernel, kLockWeapon, &too_small));
    Kernel_Destroy(kernel);
}

void a_lock_weapon_whose_cone_is_not_a_cone_rejects_the_catalog() {
    KernelHandle* kernel = create_client();
    require(!load(kernel, KernelColliderShapeType_Sphere, lock_weapon()));
    KernelWeaponMechanicsDefinition read{};
    read.struct_size = sizeof(read);
    require(!Kernel_GetCatalogWeaponDefinition(kernel, kLockWeapon, &read));
    Kernel_Destroy(kernel);
}

void a_lock_weapon_naming_an_unknown_fire_action_rejects_the_catalog() {
    KernelHandle* kernel = create_client();
    KernelWeaponMechanicsDefinition weapon = lock_weapon();
    weapon.fire_action_template_id = 9999u;
    require(!load(kernel, KernelColliderShapeType_Cone, weapon));
    Kernel_Destroy(kernel);
}

}  // namespace

int main() {
    a_pure_client_reads_a_catalog_weapon_back();
    a_lock_weapon_whose_cone_is_not_a_cone_rejects_the_catalog();
    a_lock_weapon_naming_an_unknown_fire_action_rejects_the_catalog();
    std::puts("catalog_weapon_definition_test passed");
    return 0;
}
