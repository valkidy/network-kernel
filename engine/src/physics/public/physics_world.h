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
    bool enable_query_stats = false;
};

struct CollisionQueryStats {
    std::uint64_t ray_query_count = 0;
    std::uint64_t shape_cast_query_count = 0;
    std::uint64_t overlap_query_count = 0;
    std::uint64_t broadphase_layer_filter_checks = 0;
    std::uint64_t broadphase_layers_accepted = 0;
    std::uint64_t damageable_actor_broadphase_layers_accepted = 0;
    std::uint64_t object_layer_filter_checks = 0;
    std::uint64_t object_layers_accepted = 0;
    std::uint64_t player_object_layers_accepted = 0;
    std::uint64_t hostile_object_layers_accepted = 0;
    std::uint64_t neutral_object_layers_accepted = 0;
    std::uint64_t raw_jolt_hits_collected = 0;
    std::uint64_t final_hits_accepted = 0;
    std::uint64_t defensive_post_filter_rejections = 0;
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
    CollisionQueryStats query_stats() const;
    void reset_query_stats();

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

    bool upsert_character(
        const CharacterDescriptor& character,
        std::string* error);
    bool remove_character(std::uint32_t character_id);
    bool move_character(
        const CharacterMoveRequest& request,
        CharacterMoveResult* result,
        std::string* error);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace network_example::physics

#endif  // PHYSICS_PUBLIC_PHYSICS_WORLD_H_
