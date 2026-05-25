#ifndef WORLD_PUBLIC_COMPONENTS_H_
#define WORLD_PUBLIC_COMPONENTS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace network_example {

using NetId = std::uint32_t;
using PeerId = std::uint32_t;

enum class EntityType : std::uint16_t {
    kUnknown = 0,
    kPlayer = 1,
    kEnemy = 2,
    kProjectile = 3,
    kAreaEffect = 4,
};

struct NetworkIdentity {
    NetId net_id = 0;
    PeerId owner_peer = 0;
};

struct EntityKind {
    EntityType type = EntityType::kUnknown;
};

struct Transform {
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct Velocity {
    glm::vec3 linear{0.0f, 0.0f, 0.0f};
};

struct Health {
    std::uint16_t hp = 0;
    std::uint16_t max_hp = 0;
};

struct PlayerTag {};
struct EnemyTag {};
struct ProjectileTag {};
struct AreaEffectTag {};

inline constexpr std::size_t kWeaponCount = 4;
inline constexpr std::uint8_t kWeaponSlot0 = 0;
inline constexpr std::uint8_t kWeaponSlot1 = 1;
inline constexpr std::uint8_t kWeaponSlot2 = 2;
inline constexpr std::uint8_t kWeaponSlot3 = 3;

enum class WeaponFireMode : std::uint8_t {
    kHitscan = 0,
    kShotgun = 1,
    kProjectile = 2,
};

enum class ProjectileMotionModel : std::uint8_t {
    kLinear = 0,
    kParabolic = 1,
};

struct WeaponState {
    std::uint8_t weapon_id = 0;
    std::array<std::uint16_t, kWeaponCount> ammo{0, 0, 0, 0};
    std::array<std::uint16_t, kWeaponCount> reserve_ammo{0, 0, 0, 0};
    std::array<std::uint32_t, kWeaponCount> next_fire_tick{0, 0, 0, 0};
    std::uint32_t reload_end_tick = 0;
    bool is_reloading = false;
};

struct WeaponMechanicsDefinition {
    std::uint8_t id = 0;
    WeaponFireMode mode = WeaponFireMode::kHitscan;
    std::uint16_t magazine_size = 0;
    std::uint16_t damage = 0;
    std::uint32_t cooldown_ticks = 0;
    std::uint32_t reload_ticks = 0;
    float max_range = 0.0f;
    std::uint8_t pellet_count = 1;
    float pellet_spread = 0.0f;
    float projectile_speed = 0.0f;
    float projectile_lifetime_seconds = 0.0f;
    float explosion_radius = 0.0f;
    ProjectileMotionModel projectile_motion_model = ProjectileMotionModel::kLinear;
    glm::vec3 projectile_gravity{0.0f, 0.0f, 0.0f};
};

struct WeaponTuning {
    std::array<bool, kWeaponCount> configured{false, false, false, false};
    std::array<WeaponMechanicsDefinition, kWeaponCount> definitions{};
};

struct Hitbox {
    glm::vec3 center{0.0f, 0.0f, 0.0f};
    glm::vec3 half_extents{0.5f, 0.5f, 0.5f};
    std::uint8_t hit_zone = 0;
};

struct ProjectileState {
    std::uint8_t weapon_id = 0;
    std::uint16_t damage = 0;
    std::uint32_t spawn_tick = 0;
    std::uint32_t client_action_id = 0;
    NetId shooter_net_id = 0;
    ProjectileMotionModel motion_model = ProjectileMotionModel::kLinear;
    float explosion_radius = 0.0f;
    float max_lifetime_seconds = 0.0f;
    float age_seconds = 0.0f;
    glm::vec3 spawn_position{0.0f, 0.0f, 0.0f};
    glm::vec3 initial_velocity{0.0f, 0.0f, 0.0f};
    // Future non-deterministic physics projectiles should use authoritative
    // snapshots plus render-side correction after a physics module exists.
    glm::vec3 gravity{0.0f, 0.0f, 0.0f};
    glm::vec3 previous_position{0.0f, 0.0f, 0.0f};
};

struct AreaEffectState {
    float radius = 0.0f;
    std::uint16_t damage_per_interval = 0;
    std::uint32_t damage_interval_ticks = 1;
    std::uint32_t expire_tick = 0;
    std::uint8_t source_code = 0;
    std::unordered_map<NetId, std::uint32_t> next_damage_tick_by_target;
};

struct MovementState {
    float speed_meters_per_second = 0.0f;
};

inline constexpr std::uint32_t kVisualFlagMoving = 0x00000001u;
inline constexpr std::uint32_t kVisualFlagReloading = 0x00000002u;
inline constexpr std::uint32_t kVisualFlagDead = 0x00000004u;

struct ReplicationState {
    std::uint16_t animation_state = 0;
    std::uint32_t visual_flags = 0;
};

}  // namespace network_example

#endif  // WORLD_PUBLIC_COMPONENTS_H_
