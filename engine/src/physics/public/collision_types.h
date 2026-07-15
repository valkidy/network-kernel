#ifndef PHYSICS_PUBLIC_COLLISION_TYPES_H_
#define PHYSICS_PUBLIC_COLLISION_TYPES_H_

#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace network_example::physics {

enum class CollisionLayer : std::uint32_t {
    kNone = 0,
    kTerrain = 1u << 0u,
    kStaticObstacle = 1u << 1u,
    kDamageable = 1u << 2u,
};

constexpr std::uint32_t collision_layer_bit(CollisionLayer layer) {
    return static_cast<std::uint32_t>(layer);
}

inline constexpr std::uint32_t kCollisionMaskAll =
    collision_layer_bit(CollisionLayer::kTerrain) |
    collision_layer_bit(CollisionLayer::kStaticObstacle) |
    collision_layer_bit(CollisionLayer::kDamageable);

inline constexpr std::uint32_t kGameplayCategoryPlayerSide = 0x00000001u;
inline constexpr std::uint32_t kGameplayCategoryHostileSide = 0x00000002u;
inline constexpr std::uint32_t kGameplayCategoryNeutral = 0x00000020u;
inline constexpr std::uint32_t kGameplayCategoryDamageable =
    kGameplayCategoryPlayerSide |
    kGameplayCategoryHostileSide |
    kGameplayCategoryNeutral;

enum class CollisionObjectKind : std::uint8_t {
    kTerrain,
    kStaticObstacle,
    kActorHitbox,
};

enum class CollisionShapeType : std::uint8_t {
    kBox,
    kSphere,
};

struct CollisionShapeDescriptor {
    CollisionShapeType type = CollisionShapeType::kBox;
    glm::vec3 local_center{0.0f};
    glm::vec3 half_extents{0.5f};
    float radius = 0.5f;
};

struct CollisionObjectIdentity {
    std::uint32_t entity_net_id = 0;
    std::uint32_t collider_id = 0;
    std::uint32_t hit_zone = 0;
    CollisionObjectKind kind = CollisionObjectKind::kTerrain;
    CollisionLayer layer = CollisionLayer::kNone;
    std::uint32_t scene_id = 0;
    std::uint32_t gameplay_category = 0;
};

struct CollisionObjectDescriptor {
    CollisionObjectIdentity identity{};
    CollisionShapeDescriptor shape{};
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    bool enabled = true;
};

struct CollisionQueryFilter {
    std::uint32_t collision_mask = kCollisionMaskAll;
    std::uint32_t ignored_entity_net_id = 0;
    std::uint32_t ignored_collider_id = 0;
    std::uint32_t object_kind_mask = 0xffffffffu;
    std::uint32_t gameplay_category_mask = 0xffffffffu;
};

struct CollisionHit {
    CollisionObjectIdentity identity{};
    float distance = 0.0f;
    float fraction = 0.0f;
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    std::uint32_t subshape_id = 0;
};

struct RayCastRequest {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{1.0f, 0.0f, 0.0f};
    float max_distance = 0.0f;
    CollisionQueryFilter filter{};
};

struct ShapeCastRequest {
    CollisionShapeDescriptor shape{};
    glm::vec3 start{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 displacement{0.0f};
    CollisionQueryFilter filter{};
};

struct OverlapRequest {
    CollisionShapeDescriptor shape{};
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    CollisionQueryFilter filter{};
};

}  // namespace network_example::physics

#endif  // PHYSICS_PUBLIC_COLLISION_TYPES_H_
