#ifndef WORLD_PUBLIC_COMPONENTS_H_
#define WORLD_PUBLIC_COMPONENTS_H_

#include <array>
#include <cstddef>
#include <cstdint>

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

inline constexpr std::uint8_t kWeaponRifle = 0;
inline constexpr std::uint8_t kWeaponShotgun = 1;
inline constexpr std::uint8_t kWeaponGrenade = 2;
inline constexpr std::size_t kWeaponCount = 3;

struct WeaponState {
    std::uint8_t weapon_id = 0;
    std::array<std::uint16_t, kWeaponCount> ammo{30, 8, 1};
    std::array<std::uint16_t, kWeaponCount> reserve_ammo{90, 32, 3};
    std::array<std::uint32_t, kWeaponCount> next_fire_tick{0, 0, 0};
    std::uint32_t reload_end_tick = 0;
    bool is_reloading = false;
};

struct Hitbox {
    glm::vec3 center{0.0f, 0.0f, 0.0f};
    glm::vec3 half_extents{0.5f, 0.5f, 0.5f};
    std::uint8_t hit_zone = 0;
};

struct ProjectileState {
    std::uint8_t weapon_id = 0;
    std::uint16_t damage = 0;
    float explosion_radius = 0.0f;
    float max_lifetime_seconds = 0.0f;
    float age_seconds = 0.0f;
    glm::vec3 previous_position{0.0f, 0.0f, 0.0f};
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
