#ifndef PHYSICS_PUBLIC_PHYSICS_WORLD_H_
#define PHYSICS_PUBLIC_PHYSICS_WORLD_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "physics/public/collision_types.h"

namespace network_example::physics {

struct PhysicsWorldConfig {
    std::uint32_t query_worker_count = 0;
};

class PhysicsWorld final {
public:
    explicit PhysicsWorld(const PhysicsWorldConfig& config = {});
    ~PhysicsWorld();

    PhysicsWorld(PhysicsWorld&&) noexcept;
    PhysicsWorld& operator=(PhysicsWorld&&) noexcept;
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    bool valid() const;
    std::uint32_t query_worker_count() const;

    bool load_static_scene(
        std::span<const std::uint8_t> artifact,
        const CollisionObjectIdentity& identity,
        std::string* error);
    bool upsert_object(const CollisionObjectDescriptor& object, std::string* error);
    bool remove_object(std::uint32_t collider_id);
    bool set_object_enabled(std::uint32_t collider_id, bool enabled);
    bool set_object_transform(
        std::uint32_t collider_id,
        const glm::vec3& position,
        const glm::quat& rotation);

    std::vector<CollisionHit> ray_cast_all(const RayCastRequest& request) const;
    std::vector<CollisionHit> shape_cast_all(const ShapeCastRequest& request) const;
    std::vector<CollisionHit> overlap_all(const OverlapRequest& request) const;
    bool ray_cast_closest(const RayCastRequest& request, CollisionHit* hit) const;
    bool shape_cast_closest(const ShapeCastRequest& request, CollisionHit* hit) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace network_example::physics

#endif  // PHYSICS_PUBLIC_PHYSICS_WORLD_H_
