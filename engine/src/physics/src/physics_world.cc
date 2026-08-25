#include "physics/public/physics_world.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace network_example::physics {
namespace {

constexpr std::array<std::uint8_t, 8> kJoltMagic = {
    'N', 'K', 'M', 'J', 'O', 'L', 'T', '1'};
constexpr std::uint32_t kArtifactSchemaVersion = 1;
constexpr std::size_t kArtifactHeaderSize = 88;
constexpr std::uint32_t kLittleEndian = 1;
constexpr std::uint32_t kBigEndian = 2;
constexpr JPH::ObjectLayer kTerrainObjectLayer = 0;
constexpr JPH::ObjectLayer kStaticObstacleObjectLayer = 1;
constexpr JPH::ObjectLayer kDamageablePlayerObjectLayer = 2;
constexpr JPH::ObjectLayer kDamageableHostileObjectLayer = 3;
constexpr JPH::ObjectLayer kDamageableNeutralObjectLayer = 4;
constexpr JPH::ObjectLayer kDamageableUnclassifiedObjectLayer = 5;
constexpr JPH::ObjectLayer kOtherMovingObjectLayer = 6;
constexpr JPH::ObjectLayer kActorLimbObjectLayer = 7;
constexpr JPH::BroadPhaseLayer kStaticWorldBroadPhaseLayer(0);
constexpr JPH::BroadPhaseLayer kDamageableActorBroadPhaseLayer(1);
constexpr JPH::BroadPhaseLayer kOtherMovingBroadPhaseLayer(2);
// Limbs get a broad phase layer to themselves rather than joining the other
// moving bodies. A legged rig contributes a dozen bodies against one movement
// capsule, so folding them in would make every foothold ray and every movement
// sweep walk that subtree; alone, they cost one ShouldCollide to skip entirely.
constexpr JPH::BroadPhaseLayer kActorLimbBroadPhaseLayer(3);

std::uint64_t jolt_version_id() {
    using JPH::uint64;
    return JPH_VERSION_ID;
}

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    std::uint32_t GetNumBroadPhaseLayers() const override { return 4; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        if (layer == kTerrainObjectLayer) {
            return kStaticWorldBroadPhaseLayer;
        }
        if (layer == kDamageablePlayerObjectLayer ||
            layer == kDamageableHostileObjectLayer ||
            layer == kDamageableNeutralObjectLayer) {
            return kDamageableActorBroadPhaseLayer;
        }
        if (layer == kActorLimbObjectLayer) {
            return kActorLimbBroadPhaseLayer;
        }
        return kOtherMovingBroadPhaseLayer;
    }
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(
        JPH::ObjectLayer /*layer1*/,
        JPH::BroadPhaseLayer /*layer2*/) const override {
        return true;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(
        JPH::ObjectLayer /*layer1*/,
        JPH::ObjectLayer /*layer2*/) const override {
        return true;
    }
};

bool kind_enabled(
    const CollisionQueryFilter& filter,
    CollisionObjectKind kind) {
    const std::uint32_t bit = 1u << static_cast<std::uint32_t>(kind);
    return (filter.object_kind_mask & bit) != 0;
}

class QueryBroadPhaseLayerFilter final : public JPH::BroadPhaseLayerFilter {
public:
    QueryBroadPhaseLayerFilter(
        const CollisionQueryFilter& filter,
        bool has_unclassified_damageable,
        CollisionQueryStats* stats)
        : filter_(filter),
          has_unclassified_damageable_(has_unclassified_damageable),
          stats_(stats) {}

    bool ShouldCollide(JPH::BroadPhaseLayer layer) const override {
        if (stats_ != nullptr) {
            ++stats_->broadphase_layer_filter_checks;
        }
        bool accepted = false;
        if (layer == kStaticWorldBroadPhaseLayer) {
            accepted =
                (filter_.collision_mask &
                 collision_layer_bit(CollisionLayer::kTerrain)) != 0 &&
                kind_enabled(filter_, CollisionObjectKind::kTerrain);
        } else if (layer == kDamageableActorBroadPhaseLayer) {
            accepted =
                (filter_.collision_mask &
                 collision_layer_bit(CollisionLayer::kDamageable)) != 0 &&
                (filter_.gameplay_category_mask &
                 kGameplayCategoryDamageable) != 0 &&
                kind_enabled(filter_, CollisionObjectKind::kActorHitbox);
        } else if (layer == kOtherMovingBroadPhaseLayer) {
            const bool accepts_static =
                (filter_.collision_mask &
                 collision_layer_bit(CollisionLayer::kStaticObstacle)) != 0 &&
                kind_enabled(filter_, CollisionObjectKind::kStaticObstacle);
            const bool accepts_unclassified = has_unclassified_damageable_ &&
                (filter_.collision_mask &
                 collision_layer_bit(CollisionLayer::kDamageable)) != 0 &&
                kind_enabled(filter_, CollisionObjectKind::kActorHitbox);
            const bool accepts_movement =
                (filter_.collision_mask &
                 collision_layer_bit(CollisionLayer::kActorMovement)) != 0 &&
                kind_enabled(filter_, CollisionObjectKind::kActorMovement);
            accepted = accepts_static || accepts_unclassified || accepts_movement;
        } else if (layer == kActorLimbBroadPhaseLayer) {
            accepted =
                (filter_.collision_mask &
                 collision_layer_bit(CollisionLayer::kActorLimb)) != 0 &&
                kind_enabled(filter_, CollisionObjectKind::kActorLimb);
        }
        if (stats_ != nullptr && accepted) {
            ++stats_->broadphase_layers_accepted;
            if (layer == kDamageableActorBroadPhaseLayer) {
                ++stats_->damageable_actor_broadphase_layers_accepted;
            } else if (layer == kActorLimbBroadPhaseLayer) {
                ++stats_->actor_limb_broadphase_layers_accepted;
            }
        }
        return accepted;
    }

private:
    const CollisionQueryFilter& filter_;
    bool has_unclassified_damageable_ = false;
    CollisionQueryStats* stats_ = nullptr;
};

class QueryObjectLayerFilter final : public JPH::ObjectLayerFilter {
public:
    QueryObjectLayerFilter(
        const CollisionQueryFilter& filter,
        CollisionQueryStats* stats)
        : filter_(filter), stats_(stats) {}

    bool ShouldCollide(JPH::ObjectLayer layer) const override {
        if (stats_ != nullptr) {
            ++stats_->object_layer_filter_checks;
        }
        bool accepted = false;
        if (layer == kTerrainObjectLayer) {
            accepted = accepts_physical(
                CollisionLayer::kTerrain, CollisionObjectKind::kTerrain);
        } else if (layer == kStaticObstacleObjectLayer) {
            accepted = accepts_physical(
                CollisionLayer::kStaticObstacle,
                CollisionObjectKind::kStaticObstacle);
        } else if (layer == kDamageablePlayerObjectLayer) {
            accepted = accepts_damageable(kGameplayCategoryPlayerSide);
        } else if (layer == kDamageableHostileObjectLayer) {
            accepted = accepts_damageable(kGameplayCategoryHostileSide);
        } else if (layer == kDamageableNeutralObjectLayer) {
            accepted = accepts_damageable(kGameplayCategoryNeutral);
        } else if (layer == kDamageableUnclassifiedObjectLayer) {
            accepted = accepts_physical(
                CollisionLayer::kDamageable,
                CollisionObjectKind::kActorHitbox);
        } else if (layer == kOtherMovingObjectLayer) {
            accepted = accepts_physical(
                CollisionLayer::kActorMovement,
                CollisionObjectKind::kActorMovement);
        } else if (layer == kActorLimbObjectLayer) {
            accepted = accepts_physical(
                CollisionLayer::kActorLimb, CollisionObjectKind::kActorLimb);
        }
        if (stats_ != nullptr && accepted) {
            ++stats_->object_layers_accepted;
            if (layer == kDamageablePlayerObjectLayer) {
                ++stats_->player_object_layers_accepted;
            } else if (layer == kDamageableHostileObjectLayer) {
                ++stats_->hostile_object_layers_accepted;
            } else if (layer == kDamageableNeutralObjectLayer) {
                ++stats_->neutral_object_layers_accepted;
            } else if (layer == kActorLimbObjectLayer) {
                ++stats_->actor_limb_object_layers_accepted;
            }
        }
        return accepted;
    }

private:
    bool accepts_physical(
        CollisionLayer layer,
        CollisionObjectKind kind) const {
        return (filter_.collision_mask & collision_layer_bit(layer)) != 0 &&
            kind_enabled(filter_, kind);
    }

    bool accepts_damageable(std::uint32_t gameplay_category) const {
        return accepts_physical(
                   CollisionLayer::kDamageable,
                   CollisionObjectKind::kActorHitbox) &&
            (filter_.gameplay_category_mask & gameplay_category) != 0;
    }

    const CollisionQueryFilter& filter_;
    CollisionQueryStats* stats_ = nullptr;
};

JPH::ObjectLayer object_layer_for(const CollisionObjectIdentity& identity) {
    if (identity.layer == CollisionLayer::kTerrain) {
        return kTerrainObjectLayer;
    }
    if (identity.layer == CollisionLayer::kStaticObstacle) {
        return kStaticObstacleObjectLayer;
    }
    if (identity.layer == CollisionLayer::kActorLimb) {
        return kActorLimbObjectLayer;
    }
    if (identity.layer != CollisionLayer::kDamageable) {
        return kOtherMovingObjectLayer;
    }
    const std::uint32_t damageable_categories =
        identity.gameplay_category & kGameplayCategoryDamageable;
    if (std::popcount(damageable_categories) != 1) {
        return kDamageableUnclassifiedObjectLayer;
    }
    if (damageable_categories == kGameplayCategoryPlayerSide) {
        return kDamageablePlayerObjectLayer;
    }
    if (damageable_categories == kGameplayCategoryHostileSide) {
        return kDamageableHostileObjectLayer;
    }
    return kDamageableNeutralObjectLayer;
}

class ProcessRuntime final {
public:
    ProcessRuntime() {
        std::scoped_lock lock(mutex());
        if (references() == 0) {
            JPH::RegisterDefaultAllocator();
            if (!JPH::VerifyJoltVersionID() || JPH::Factory::sInstance != nullptr) {
                return;
            }
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
        ++references();
        acquired_ = true;
    }

    ~ProcessRuntime() {
        std::scoped_lock lock(mutex());
        if (!acquired_ || --references() != 0) {
            return;
        }
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    bool acquired() const { return acquired_; }

private:
    static std::mutex& mutex() {
        static std::mutex value;
        return value;
    }

    static std::uint32_t& references() {
        static std::uint32_t value = 0;
        return value;
    }

    bool acquired_ = false;
};

std::uint32_t native_endianness() {
    return std::endian::native == std::endian::little
        ? kLittleEndian
        : kBigEndian;
}

bool read_u32(
    std::span<const std::uint8_t> bytes,
    std::size_t* offset,
    std::uint32_t* value) {
    if (*offset + sizeof(*value) > bytes.size()) {
        return false;
    }
    *value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        *value |= static_cast<std::uint32_t>(bytes[(*offset)++]) << shift;
    }
    return true;
}

bool read_u64(
    std::span<const std::uint8_t> bytes,
    std::size_t* offset,
    std::uint64_t* value) {
    if (*offset + sizeof(*value) > bytes.size()) {
        return false;
    }
    *value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        *value |= static_cast<std::uint64_t>(bytes[(*offset)++]) << shift;
    }
    return true;
}

bool validate_artifact(
    std::span<const std::uint8_t> artifact,
    std::uint64_t* payload_size,
    std::string* error) {
    if (artifact.size() < kArtifactHeaderSize ||
        !std::equal(kJoltMagic.begin(), kJoltMagic.end(), artifact.begin())) {
        *error = "invalid Jolt mesh artifact magic or truncated header";
        return false;
    }
    std::size_t offset = kJoltMagic.size();
    std::uint32_t schema_version = 0;
    std::uint32_t header_size = 0;
    std::uint64_t backend_version = 0;
    std::uint32_t endianness = 0;
    std::uint32_t ignored = 0;
    std::uint32_t reserved = 0;
    std::uint64_t ignored64 = 0;
    if (!read_u32(artifact, &offset, &schema_version) ||
        !read_u32(artifact, &offset, &header_size) ||
        !read_u64(artifact, &offset, &backend_version) ||
        !read_u32(artifact, &offset, &endianness) ||
        !read_u32(artifact, &offset, &ignored) ||
        !read_u32(artifact, &offset, &ignored) ||
        !read_u32(artifact, &offset, &reserved) ||
        !read_u64(artifact, &offset, &ignored64) ||
        !read_u64(artifact, &offset, &ignored64) ||
        !read_u64(artifact, &offset, payload_size)) {
        *error = "truncated Jolt mesh artifact header";
        return false;
    }
    if (schema_version != kArtifactSchemaVersion ||
        header_size != kArtifactHeaderSize || reserved != 0) {
        *error = "unsupported Jolt mesh artifact schema";
        return false;
    }
    if (backend_version != jolt_version_id()) {
        *error = "Jolt mesh artifact backend version mismatch";
        return false;
    }
    if ((endianness != kLittleEndian && endianness != kBigEndian) ||
        endianness != native_endianness()) {
        *error = "Jolt mesh artifact endianness mismatch";
        return false;
    }
    if (*payload_size != artifact.size() - kArtifactHeaderSize) {
        *error = "Jolt mesh artifact payload size mismatch";
        return false;
    }
    return true;
}

JPH::Vec3 to_jolt(const glm::vec3& value) {
    return JPH::Vec3(value.x, value.y, value.z);
}

JPH::RVec3 to_jolt_r(const glm::vec3& value) {
    return JPH::RVec3(value.x, value.y, value.z);
}

JPH::Quat to_jolt(const glm::quat& value) {
    return JPH::Quat(value.x, value.y, value.z, value.w).Normalized();
}

glm::vec3 from_jolt(JPH::Vec3Arg value) {
    return glm::vec3(value.GetX(), value.GetY(), value.GetZ());
}

bool valid_shape(const CollisionShapeDescriptor& shape) {
    if (shape.type == CollisionShapeType::kSphere) {
        return std::isfinite(shape.radius) && shape.radius > 0.0f;
    }
    if (shape.type == CollisionShapeType::kCapsule) {
        return std::isfinite(shape.radius) && shape.radius > 0.0f &&
            std::isfinite(shape.capsule_half_height) &&
            shape.capsule_half_height > 0.0f;
    }
    return std::isfinite(shape.half_extents.x) &&
           std::isfinite(shape.half_extents.y) &&
           std::isfinite(shape.half_extents.z) &&
           shape.half_extents.x > 0.0f &&
           shape.half_extents.y > 0.0f &&
           shape.half_extents.z > 0.0f;
}

// Two descriptors that produce an identical Jolt shape. Exact float comparison
// is intentional: descriptors are copied verbatim from collider templates, so a
// shape that did not change compares bit-identical and a shape that did change
// must force the body to be recreated.
bool same_shape(
    const CollisionShapeDescriptor& lhs,
    const CollisionShapeDescriptor& rhs) {
    return lhs.type == rhs.type && lhs.local_center == rhs.local_center &&
           lhs.half_extents == rhs.half_extents && lhs.radius == rhs.radius &&
           lhs.capsule_half_height == rhs.capsule_half_height;
}

JPH::RefConst<JPH::Shape> make_shape(const CollisionShapeDescriptor& shape) {
    if (!valid_shape(shape)) {
        return nullptr;
    }
    if (shape.type == CollisionShapeType::kSphere) {
        return new JPH::SphereShape(shape.radius);
    }
    if (shape.type == CollisionShapeType::kCapsule) {
        return new JPH::CapsuleShape(
            shape.capsule_half_height,
            shape.radius);
    }
    return new JPH::BoxShape(to_jolt(shape.half_extents));
}

std::uint32_t body_key(const JPH::BodyID& body_id) {
    return body_id.GetIndexAndSequenceNumber();
}

bool hit_less(const CollisionHit& lhs, const CollisionHit& rhs) {
    if (lhs.distance != rhs.distance) {
        return lhs.distance < rhs.distance;
    }
    if (lhs.identity.entity_net_id != rhs.identity.entity_net_id) {
        return lhs.identity.entity_net_id < rhs.identity.entity_net_id;
    }
    if (lhs.identity.collider_id != rhs.identity.collider_id) {
        return lhs.identity.collider_id < rhs.identity.collider_id;
    }
    return lhs.subshape_id < rhs.subshape_id;
}

bool filter_accepts(
    const CollisionObjectIdentity& identity,
    const CollisionQueryFilter& filter) {
    const std::uint32_t kind_bit =
        1u << static_cast<std::uint32_t>(identity.kind);
    // Category filtering is symmetric: an object that declares no category is
    // visible to every query -- that is how terrain is solid to all sides -- and
    // a query that names no category is constrained only by layer and kind.
    //
    // Without the second half, a projectile authored to hit the world rather
    // than a side (collision_mask terrain|static_obstacle) ends up with an empty
    // gameplay_category_mask, because collision_filter_from_mask only forwards
    // ACTOR bits into it. Such a query could then reach terrain, whose category
    // is zero, but passed straight through any prop that declared one -- so
    // deployable cover stopped every projectile that named a side and none of
    // the ones that named none. Layer and kind still gate what it can reach, so
    // naming no category widens nothing beyond what the mask already asked for.
    return (filter.collision_mask & collision_layer_bit(identity.layer)) != 0 &&
           (filter.object_kind_mask & kind_bit) != 0 &&
           (identity.gameplay_category == 0 ||
            filter.gameplay_category_mask == 0 ||
            (filter.gameplay_category_mask & identity.gameplay_category) != 0) &&
           (filter.ignored_entity_net_id == 0 ||
            filter.ignored_entity_net_id != identity.entity_net_id) &&
           (filter.ignored_collider_id == 0 ||
            filter.ignored_collider_id != identity.collider_id);
}

}  // namespace

class PhysicsWorld::Impl final {
public:
    explicit Impl(const PhysicsWorldConfig& config)
        : runtime_(),
          worker_count_(config.query_worker_count),
          query_stats_enabled_(config.enable_query_stats) {
        if (!runtime_.acquired()) {
            return;
        }
        if (worker_count_ > 0) {
            workers_ = std::make_unique<JPH::JobSystemThreadPool>(
                JPH::cMaxPhysicsJobs,
                JPH::cMaxPhysicsBarriers,
                static_cast<int>(worker_count_));
        }
        system_ = std::make_unique<JPH::PhysicsSystem>();
        system_->Init(
            4096,
            0,
            4096,
            1024,
            broad_phase_layers_,
            object_vs_broad_phase_filter_,
            object_layer_pair_filter_);
    }

    ~Impl() {
        if (system_ == nullptr) {
            return;
        }
        characters_.clear();
        JPH::BodyInterface& bodies = system_->GetBodyInterface();
        for (const auto& [collider_id, body_id] : bodies_by_collider_) {
            (void)collider_id;
            if (bodies.IsAdded(body_id)) {
                bodies.RemoveBody(body_id);
            }
            bodies.DestroyBody(body_id);
        }
    }

    struct StoredObject {
        CollisionObjectIdentity identity{};
        JPH::ObjectLayer object_layer = kOtherMovingObjectLayer;
        // Shape the body was created from, so upsert_object can recognise an
        // unchanged shape and refresh the body in place. Only bodies created
        // through upsert_object carry one; load_static_scene restores a mesh
        // shape that has no descriptor form and leaves shape_tracked false so it
        // never takes the in-place path.
        CollisionShapeDescriptor shape{};
        bool shape_tracked = false;
    };

    struct StoredCharacter {
        CharacterDescriptor descriptor{};
        JPH::Ref<JPH::CharacterVirtual> character;
    };

    class CharacterBodyFilter final : public JPH::BodyFilter {
    public:
        CharacterBodyFilter(
            const Impl& impl,
            const CollisionQueryFilter& filter)
            : impl_(impl), filter_(filter) {}

        bool ShouldCollide(const JPH::BodyID& body_id) const override {
            const StoredObject* object = impl_.find(body_id);
            return object != nullptr && filter_accepts(object->identity, filter_);
        }

        bool ShouldCollideLocked(const JPH::Body& body) const override {
            return ShouldCollide(body.GetID());
        }

    private:
        const Impl& impl_;
        const CollisionQueryFilter& filter_;
    };

    bool valid() const { return system_ != nullptr; }

    bool add_body(
        const JPH::Shape* shape,
        const CollisionObjectIdentity& identity,
        const glm::vec3& position,
        const glm::quat& rotation,
        JPH::EMotionType motion_type,
        JPH::ObjectLayer object_layer,
        std::string* error,
        const CollisionShapeDescriptor* tracked_shape = nullptr) {
        if (!valid() || identity.collider_id == 0 || shape == nullptr) {
            *error = "invalid collision object";
            return false;
        }
        remove_object(identity.collider_id);
        JPH::BodyInterface& bodies = system_->GetBodyInterface();
        const JPH::BodyID body_id = bodies.CreateAndAddBody(
            JPH::BodyCreationSettings(
                shape,
                to_jolt_r(position),
                to_jolt(rotation),
                motion_type,
                object_layer),
            JPH::EActivation::DontActivate);
        if (body_id.IsInvalid()) {
            *error = "Jolt body creation failed";
            return false;
        }
        bodies_by_collider_.emplace(identity.collider_id, body_id);
        StoredObject stored{identity, object_layer};
        if (tracked_shape != nullptr) {
            stored.shape = *tracked_shape;
            stored.shape_tracked = true;
        }
        objects_by_body_.emplace(body_key(body_id), stored);
        if (object_layer == kDamageableUnclassifiedObjectLayer) {
            ++unclassified_damageable_count_;
        }
        // The body entered the broad phase, so the tree owes us a rebuild before
        // its node allocator is exhausted. See optimize_broad_phase().
        broad_phase_dirty_ = true;
        return true;
    }

    // Refreshes an existing body in place instead of destroying and recreating
    // it. Returns false when the caller must fall back to a full recreate,
    // i.e. when there is no such body yet or its shape / object layer changed.
    //
    // This exists because recreating a body permanently burns a Jolt broad phase
    // node. QuadTree::RemoveBodies only clears the child slot the body occupied
    // and never returns the node to the allocator, while
    // QuadTree::AddBodiesFinalize probes just the four child slots of the
    // *current root* before allocating a whole new root node. Nodes come back
    // only via the tree rebuild in QuadTree::DiscardOldTree. So once a world
    // holds enough bodies that the freed slots sit in nodes below the root --
    // which is every real world -- remove+add churn drains the allocator at
    // roughly one node per three adds until Jolt gives up and calls std::abort()
    // with "QuadTree: Out of nodes!". KernelEngine::sync_client_render_colliders
    // re-upserts every tracked collider every frame, which used to make that
    // abort a matter of minutes.
    bool refresh_object(
        const CollisionObjectDescriptor& object,
        const glm::vec3& world_position,
        JPH::ObjectLayer object_layer) {
        const auto found = bodies_by_collider_.find(object.identity.collider_id);
        if (found == bodies_by_collider_.end()) {
            return false;
        }
        const auto stored = objects_by_body_.find(body_key(found->second));
        if (stored == objects_by_body_.end() || !stored->second.shape_tracked ||
            stored->second.object_layer != object_layer ||
            !same_shape(stored->second.shape, object.shape)) {
            return false;
        }
        JPH::BodyInterface& bodies = system_->GetBodyInterface();
        // Moving a body only widens broad phase bounds, it never allocates.
        bodies.SetPositionAndRotation(
            found->second,
            to_jolt_r(world_position),
            to_jolt(object.rotation),
            JPH::EActivation::DontActivate);
        // Toggling enabled does re-enter the broad phase and therefore does cost
        // a node, but unlike the transform refresh above it only happens when
        // the object actually changes state.
        if (object.enabled && !bodies.IsAdded(found->second)) {
            bodies.AddBody(found->second, JPH::EActivation::DontActivate);
            broad_phase_dirty_ = true;
        } else if (!object.enabled && bodies.IsAdded(found->second)) {
            bodies.RemoveBody(found->second);
            broad_phase_dirty_ = true;
        }
        // Identity fields (hit zone, gameplay category, ...) are pure query
        // filter data and can be refreshed without touching the body.
        stored->second.identity = object.identity;
        return true;
    }

    // Rebuilds the broad phase tree and hands the retired nodes back to the
    // allocator. A simulated world gets this for free from PhysicsSystem::Update,
    // but this world is query-only and never steps, so without an explicit call
    // the node allocator sized by the PhysicsSystem::Init above -- roughly
    // 2 * (maxBodies / 2 + maxBodies / 6), about 5.4k nodes for the 4096 body
    // limit -- only ever drains and Jolt eventually aborts the process.
    //
    // The dirty flag keeps this affordable: OptimizeBroadPhase() is a full tree
    // rebuild, so we pay for it only on frames where bodies were added or
    // removed, not on the far more common frames that just move existing bodies.
    // Rebuilding also re-tightens node bounds, which SetPositionAndRotation only
    // ever widens -- without it query cost degrades over a long session.
    void optimize_broad_phase() {
        if (!valid() || !broad_phase_dirty_) {
            return;
        }
        system_->OptimizeBroadPhase();
        broad_phase_dirty_ = false;
    }

    bool remove_object(std::uint32_t collider_id) {
        const auto found = bodies_by_collider_.find(collider_id);
        if (found == bodies_by_collider_.end()) {
            return false;
        }
        JPH::BodyInterface& bodies = system_->GetBodyInterface();
        const auto stored = objects_by_body_.find(body_key(found->second));
        if (stored != objects_by_body_.end()) {
            if (stored->second.object_layer ==
                kDamageableUnclassifiedObjectLayer) {
                --unclassified_damageable_count_;
            }
            objects_by_body_.erase(stored);
        }
        bodies.RemoveBody(found->second);
        bodies.DestroyBody(found->second);
        bodies_by_collider_.erase(found);
        // The vacated broad phase node stays allocated until the next rebuild.
        broad_phase_dirty_ = true;
        return true;
    }

    const StoredObject* find(const JPH::BodyID& body_id) const {
        const auto found = objects_by_body_.find(body_key(body_id));
        return found == objects_by_body_.end() ? nullptr : &found->second;
    }

    void normalize(std::vector<CollisionHit>* hits) const {
        std::stable_sort(hits->begin(), hits->end(), hit_less);
    }

    CollisionQueryStats* local_stats(CollisionQueryStats* stats) const {
        return query_stats_enabled_ ? stats : nullptr;
    }

    void record_stats(const CollisionQueryStats& delta) const {
        if (!query_stats_enabled_) {
            return;
        }
        std::scoped_lock lock(query_stats_mutex_);
        query_stats_.ray_query_count += delta.ray_query_count;
        query_stats_.shape_cast_query_count += delta.shape_cast_query_count;
        query_stats_.overlap_query_count += delta.overlap_query_count;
        query_stats_.broadphase_layer_filter_checks +=
            delta.broadphase_layer_filter_checks;
        query_stats_.broadphase_layers_accepted +=
            delta.broadphase_layers_accepted;
        query_stats_.damageable_actor_broadphase_layers_accepted +=
            delta.damageable_actor_broadphase_layers_accepted;
        query_stats_.actor_limb_broadphase_layers_accepted +=
            delta.actor_limb_broadphase_layers_accepted;
        query_stats_.object_layer_filter_checks +=
            delta.object_layer_filter_checks;
        query_stats_.object_layers_accepted += delta.object_layers_accepted;
        query_stats_.player_object_layers_accepted +=
            delta.player_object_layers_accepted;
        query_stats_.hostile_object_layers_accepted +=
            delta.hostile_object_layers_accepted;
        query_stats_.neutral_object_layers_accepted +=
            delta.neutral_object_layers_accepted;
        query_stats_.actor_limb_object_layers_accepted +=
            delta.actor_limb_object_layers_accepted;
        query_stats_.raw_jolt_hits_collected +=
            delta.raw_jolt_hits_collected;
        query_stats_.final_hits_accepted += delta.final_hits_accepted;
        query_stats_.defensive_post_filter_rejections +=
            delta.defensive_post_filter_rejections;
    }

    CollisionQueryStats query_stats() const {
        std::scoped_lock lock(query_stats_mutex_);
        return query_stats_;
    }

    void reset_query_stats() {
        std::scoped_lock lock(query_stats_mutex_);
        query_stats_ = {};
    }

    ProcessRuntime runtime_;
    std::uint32_t worker_count_ = 0;
    bool query_stats_enabled_ = false;
    std::unique_ptr<JPH::JobSystemThreadPool> workers_;
    BroadPhaseLayers broad_phase_layers_;
    ObjectVsBroadPhaseFilter object_vs_broad_phase_filter_;
    ObjectLayerPairFilter object_layer_pair_filter_;
    std::unique_ptr<JPH::PhysicsSystem> system_;
    std::unordered_map<std::uint32_t, StoredCharacter> characters_;
    JPH::TempAllocatorImpl character_allocator_{4 * 1024 * 1024};
    std::unordered_map<std::uint32_t, JPH::BodyID> bodies_by_collider_;
    std::unordered_map<std::uint32_t, StoredObject> objects_by_body_;
    std::size_t unclassified_damageable_count_ = 0;
    // Set whenever a body enters or leaves the broad phase; cleared by
    // optimize_broad_phase().
    bool broad_phase_dirty_ = false;
    mutable std::mutex query_stats_mutex_;
    mutable CollisionQueryStats query_stats_{};
    mutable std::shared_mutex mutex_;
};

PhysicsWorld::PhysicsWorld(const PhysicsWorldConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

PhysicsWorld::~PhysicsWorld() = default;
PhysicsWorld::PhysicsWorld(PhysicsWorld&&) noexcept = default;
PhysicsWorld& PhysicsWorld::operator=(PhysicsWorld&&) noexcept = default;

bool PhysicsWorld::valid() const {
    return impl_ != nullptr && impl_->valid();
}

std::uint32_t PhysicsWorld::query_worker_count() const {
    return impl_ == nullptr ? 0 : impl_->worker_count_;
}

CollisionQueryStats PhysicsWorld::query_stats() const {
    return impl_ == nullptr ? CollisionQueryStats{} : impl_->query_stats();
}

void PhysicsWorld::reset_query_stats() {
    if (impl_ != nullptr) {
        impl_->reset_query_stats();
    }
}

bool PhysicsWorld::load_static_scene(
    std::span<const std::uint8_t> artifact,
    const CollisionObjectIdentity& identity,
    std::string* error) {
    std::string local_error;
    error = error == nullptr ? &local_error : error;
    std::unique_lock lock(impl_->mutex_);
    std::uint64_t payload_size = 0;
    if (!validate_artifact(artifact, &payload_size, error)) {
        return false;
    }
    const std::string payload(
        reinterpret_cast<const char*>(artifact.data() + kArtifactHeaderSize),
        static_cast<std::size_t>(payload_size));
    std::istringstream input(payload, std::ios::binary | std::ios::in);
    JPH::StreamInWrapper stream(input);
    JPH::Shape::IDToShapeMap shape_map;
    JPH::Shape::IDToMaterialMap material_map;
    JPH::Shape::ShapeResult result = JPH::Shape::sRestoreWithChildren(
        stream, shape_map, material_map);
    if (result.HasError() || stream.IsFailed()) {
        *error = result.HasError()
            ? "Jolt MeshShape restore failed: " + result.GetError()
            : "Jolt MeshShape payload is truncated";
        return false;
    }
    return impl_->add_body(
        result.Get(),
        identity,
        glm::vec3(0.0f),
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
        JPH::EMotionType::Static,
        kTerrainObjectLayer,
        error);
}

bool PhysicsWorld::upsert_object(
    const CollisionObjectDescriptor& object,
    std::string* error) {
    std::string local_error;
    error = error == nullptr ? &local_error : error;
    std::unique_lock lock(impl_->mutex_);
    if (!valid_shape(object.shape)) {
        *error = "invalid collision shape dimensions";
        return false;
    }
    const glm::vec3 world_position =
        object.position + object.rotation * object.shape.local_center;
    const JPH::ObjectLayer object_layer = object_layer_for(object.identity);
    // Callers such as KernelEngine::sync_client_render_colliders re-upsert every
    // tracked collider every frame. Recreating the body each time leaks a broad
    // phase node per call, so move the existing body instead whenever its shape
    // and object layer are unchanged. Validation above runs first so a bad
    // descriptor is still rejected on this path, and the Jolt shape is only
    // built when a real recreate is needed.
    if (impl_->refresh_object(object, world_position, object_layer)) {
        return true;
    }
    JPH::RefConst<JPH::Shape> shape = make_shape(object.shape);
    if (shape == nullptr) {
        *error = "invalid collision shape dimensions";
        return false;
    }
    if (!impl_->add_body(
            shape,
            object.identity,
            world_position,
            object.rotation,
            JPH::EMotionType::Kinematic,
            object_layer,
            error,
            &object.shape)) {
        return false;
    }
    if (!object.enabled) {
        const auto found =
            impl_->bodies_by_collider_.find(object.identity.collider_id);
        if (found != impl_->bodies_by_collider_.end()) {
            impl_->system_->GetBodyInterface().RemoveBody(found->second);
        }
    }
    return true;
}

bool PhysicsWorld::remove_object(std::uint32_t collider_id) {
    std::unique_lock lock(impl_->mutex_);
    return impl_->remove_object(collider_id);
}

bool PhysicsWorld::set_object_enabled(std::uint32_t collider_id, bool enabled) {
    std::unique_lock lock(impl_->mutex_);
    const auto found = impl_->bodies_by_collider_.find(collider_id);
    if (found == impl_->bodies_by_collider_.end()) {
        return false;
    }
    JPH::BodyInterface& bodies = impl_->system_->GetBodyInterface();
    if (enabled && !bodies.IsAdded(found->second)) {
        bodies.AddBody(found->second, JPH::EActivation::DontActivate);
        impl_->broad_phase_dirty_ = true;
    } else if (!enabled && bodies.IsAdded(found->second)) {
        bodies.RemoveBody(found->second);
        impl_->broad_phase_dirty_ = true;
    }
    return true;
}

void PhysicsWorld::optimize_broad_phase() {
    if (impl_ == nullptr) {
        return;
    }
    std::unique_lock lock(impl_->mutex_);
    impl_->optimize_broad_phase();
}

bool PhysicsWorld::set_object_transform(
    std::uint32_t collider_id,
    const glm::vec3& position,
    const glm::quat& rotation) {
    std::unique_lock lock(impl_->mutex_);
    const auto found = impl_->bodies_by_collider_.find(collider_id);
    if (found == impl_->bodies_by_collider_.end()) {
        return false;
    }
    impl_->system_->GetBodyInterface().SetPositionAndRotation(
        found->second,
        to_jolt_r(position),
        to_jolt(rotation),
        JPH::EActivation::DontActivate);
    return true;
}

bool PhysicsWorld::upsert_character(
    const CharacterDescriptor& descriptor,
    std::string* error) {
    std::string local_error;
    error = error == nullptr ? &local_error : error;
    std::unique_lock lock(impl_->mutex_);
    if (!valid() || descriptor.character_id == 0 ||
        descriptor.shape.type != CollisionShapeType::kCapsule ||
        !valid_shape(descriptor.shape) ||
        !std::isfinite(descriptor.max_slope_degrees) ||
        descriptor.max_slope_degrees <= 0.0f ||
        descriptor.max_slope_degrees >= 90.0f) {
        *error = "invalid CharacterVirtual descriptor";
        return false;
    }

    const auto found = impl_->characters_.find(descriptor.character_id);
    if (found != impl_->characters_.end()) {
        const CharacterDescriptor& current = found->second.descriptor;
        if (current.shape.local_center == descriptor.shape.local_center &&
            current.shape.radius == descriptor.shape.radius &&
            current.shape.capsule_half_height ==
                descriptor.shape.capsule_half_height &&
            current.max_slope_degrees == descriptor.max_slope_degrees) {
            return true;
        }
        impl_->characters_.erase(found);
    }

    JPH::RefConst<JPH::Shape> shape = make_shape(descriptor.shape);
    if (shape == nullptr) {
        *error = "CharacterVirtual capsule creation failed";
        return false;
    }
    JPH::CharacterVirtualSettings settings;
    settings.mID = JPH::CharacterID(descriptor.character_id);
    settings.mShape = shape;
    settings.mShapeOffset = to_jolt(descriptor.shape.local_center);
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(
        descriptor.max_slope_degrees);
    settings.mInnerBodyShape = nullptr;

    Impl::StoredCharacter stored;
    stored.descriptor = descriptor;
    stored.character = new JPH::CharacterVirtual(
        &settings,
        JPH::RVec3::sZero(),
        JPH::Quat::sIdentity(),
        impl_->system_.get());
    impl_->characters_.emplace(descriptor.character_id, std::move(stored));
    return true;
}

bool PhysicsWorld::remove_character(std::uint32_t character_id) {
    std::unique_lock lock(impl_->mutex_);
    return impl_->characters_.erase(character_id) != 0;
}

bool PhysicsWorld::move_character(
    const CharacterMoveRequest& request,
    CharacterMoveResult* result,
    std::string* error) {
    std::string local_error;
    error = error == nullptr ? &local_error : error;
    std::unique_lock lock(impl_->mutex_);
    if (!valid() || result == nullptr || request.character_id == 0 ||
        !std::isfinite(request.delta_seconds) || request.delta_seconds <= 0.0f ||
        !std::isfinite(request.step_height) || request.step_height < 0.0f ||
        !std::isfinite(request.ground_snap_distance) ||
        request.ground_snap_distance < 0.0f) {
        *error = "invalid CharacterVirtual move request";
        return false;
    }
    const auto found = impl_->characters_.find(request.character_id);
    if (found == impl_->characters_.end()) {
        *error = "CharacterVirtual was not created";
        return false;
    }

    JPH::CharacterVirtual& character = *found->second.character;
    character.SetPosition(to_jolt_r(request.current_position));
    character.SetRotation(to_jolt(request.current_rotation));
    character.SetLinearVelocity(to_jolt(request.linear_velocity));

    QueryBroadPhaseLayerFilter broadphase_filter(
        request.filter,
        impl_->unclassified_damageable_count_ != 0,
        nullptr);
    QueryObjectLayerFilter object_layer_filter(request.filter, nullptr);
    Impl::CharacterBodyFilter body_filter(*impl_, request.filter);
    JPH::ShapeFilter shape_filter;
    JPH::CharacterVirtual::ExtendedUpdateSettings settings;
    settings.mStickToFloorStepDown =
        JPH::Vec3(0.0f, -request.ground_snap_distance, 0.0f);
    settings.mWalkStairsStepUp = JPH::Vec3(0.0f, request.step_height, 0.0f);
    character.ExtendedUpdate(
        request.delta_seconds,
        to_jolt(request.gravity),
        settings,
        broadphase_filter,
        object_layer_filter,
        body_filter,
        shape_filter,
        impl_->character_allocator_);

    result->position = from_jolt(character.GetPosition());
    result->linear_velocity = from_jolt(character.GetLinearVelocity());
    result->ground_normal = from_jolt(character.GetGroundNormal());
    result->supporting_identity = {};
    const Impl::StoredObject* support =
        impl_->find(character.GetGroundBodyID());
    if (support != nullptr) {
        result->supporting_identity = support->identity;
    }
    switch (character.GetGroundState()) {
        case JPH::CharacterBase::EGroundState::OnGround:
            result->ground_state = CharacterGroundState::kGrounded;
            break;
        case JPH::CharacterBase::EGroundState::OnSteepGround:
            result->ground_state = CharacterGroundState::kSteepGround;
            break;
        case JPH::CharacterBase::EGroundState::NotSupported:
        case JPH::CharacterBase::EGroundState::InAir:
            result->ground_state = CharacterGroundState::kAirborne;
            break;
    }
    return true;
}

std::vector<CollisionHit> PhysicsWorld::ray_cast_all(
    const RayCastRequest& request) const {
    std::shared_lock lock(impl_->mutex_);
    CollisionQueryStats stats{};
    stats.ray_query_count = 1;
    const float direction_length = glm::length(request.direction);
    if (!valid() || request.max_distance <= 0.0f || direction_length <= 0.000001f) {
        impl_->record_stats(stats);
        return {};
    }
    const glm::vec3 displacement =
        request.direction / direction_length * request.max_distance;
    QueryBroadPhaseLayerFilter broadphase_filter(
        request.filter,
        impl_->unclassified_damageable_count_ != 0,
        impl_->local_stats(&stats));
    QueryObjectLayerFilter object_layer_filter(
        request.filter, impl_->local_stats(&stats));
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    impl_->system_->GetNarrowPhaseQuery().CastRay(
        JPH::RRayCast(to_jolt_r(request.origin), to_jolt(displacement)),
        JPH::RayCastSettings{},
        collector,
        broadphase_filter,
        object_layer_filter);
    stats.raw_jolt_hits_collected = collector.mHits.size();

    std::vector<CollisionHit> hits;
    hits.reserve(collector.mHits.size());
    for (const JPH::RayCastResult& result : collector.mHits) {
        const Impl::StoredObject* object = impl_->find(result.mBodyID);
        if (object == nullptr) {
            continue;
        }
        if (!filter_accepts(object->identity, request.filter)) {
            ++stats.defensive_post_filter_rejections;
            continue;
        }
        const glm::vec3 hit_position =
            request.origin + displacement * result.mFraction;
        glm::vec3 hit_normal = -request.direction / direction_length;
        JPH::BodyLockRead body_lock(
            impl_->system_->GetBodyLockInterface(), result.mBodyID);
        if (body_lock.Succeeded()) {
            const JPH::Body& body = body_lock.GetBody();
            const JPH::Vec3 local_hit_position = JPH::Vec3(
                to_jolt_r(hit_position) - body.GetPosition());
            hit_normal = from_jolt(
                body.GetWorldTransform().Multiply3x3(
                    body.GetShape()->GetSurfaceNormal(
                        result.mSubShapeID2,
                        local_hit_position)));
        }
        hits.push_back(CollisionHit{
            object->identity,
            result.mFraction * request.max_distance,
            result.mFraction,
            hit_position,
            hit_normal,
            result.mSubShapeID2.GetValue(),
        });
    }
    impl_->normalize(&hits);
    stats.final_hits_accepted = hits.size();
    impl_->record_stats(stats);
    return hits;
}

std::vector<CollisionHit> PhysicsWorld::shape_cast_all(
    const ShapeCastRequest& request) const {
    std::shared_lock lock(impl_->mutex_);
    CollisionQueryStats stats{};
    stats.shape_cast_query_count = 1;
    if (!valid()) {
        impl_->record_stats(stats);
        return {};
    }
    JPH::RefConst<JPH::Shape> shape = make_shape(request.shape);
    if (shape == nullptr) {
        impl_->record_stats(stats);
        return {};
    }
    const glm::vec3 start = request.start + request.rotation * request.shape.local_center;
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        shape,
        JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sRotationTranslation(
            to_jolt(request.rotation), to_jolt_r(start)),
        to_jolt(request.displacement));
    QueryBroadPhaseLayerFilter broadphase_filter(
        request.filter,
        impl_->unclassified_damageable_count_ != 0,
        impl_->local_stats(&stats));
    QueryObjectLayerFilter object_layer_filter(
        request.filter, impl_->local_stats(&stats));
    JPH::AllHitCollisionCollector<JPH::CastShapeCollector> collector;
    impl_->system_->GetNarrowPhaseQuery().CastShape(
        cast,
        JPH::ShapeCastSettings{},
        JPH::RVec3::sZero(),
        collector,
        broadphase_filter,
        object_layer_filter);
    stats.raw_jolt_hits_collected = collector.mHits.size();

    const float cast_length = glm::length(request.displacement);
    std::vector<CollisionHit> hits;
    hits.reserve(collector.mHits.size());
    for (const JPH::ShapeCastResult& result : collector.mHits) {
        const Impl::StoredObject* object = impl_->find(result.mBodyID2);
        if (object == nullptr) {
            continue;
        }
        if (!filter_accepts(object->identity, request.filter)) {
            ++stats.defensive_post_filter_rejections;
            continue;
        }
        hits.push_back(CollisionHit{
            object->identity,
            result.mFraction * cast_length,
            result.mFraction,
            from_jolt(result.mContactPointOn2),
            from_jolt(-result.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisY())),
            result.mSubShapeID2.GetValue(),
        });
    }
    impl_->normalize(&hits);
    stats.final_hits_accepted = hits.size();
    impl_->record_stats(stats);
    return hits;
}

std::vector<CollisionHit> PhysicsWorld::overlap_all(
    const OverlapRequest& request) const {
    std::shared_lock lock(impl_->mutex_);
    CollisionQueryStats stats{};
    stats.overlap_query_count = 1;
    if (!valid()) {
        impl_->record_stats(stats);
        return {};
    }
    JPH::RefConst<JPH::Shape> shape = make_shape(request.shape);
    if (shape == nullptr) {
        impl_->record_stats(stats);
        return {};
    }
    const glm::vec3 position =
        request.position + request.rotation * request.shape.local_center;
    QueryBroadPhaseLayerFilter broadphase_filter(
        request.filter,
        impl_->unclassified_damageable_count_ != 0,
        impl_->local_stats(&stats));
    QueryObjectLayerFilter object_layer_filter(
        request.filter, impl_->local_stats(&stats));
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    impl_->system_->GetNarrowPhaseQuery().CollideShape(
        shape,
        JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sRotationTranslation(
            to_jolt(request.rotation), to_jolt_r(position)),
        JPH::CollideShapeSettings{},
        JPH::RVec3::sZero(),
        collector,
        broadphase_filter,
        object_layer_filter);
    stats.raw_jolt_hits_collected = collector.mHits.size();

    std::vector<CollisionHit> hits;
    hits.reserve(collector.mHits.size());
    for (const JPH::CollideShapeResult& result : collector.mHits) {
        const Impl::StoredObject* object = impl_->find(result.mBodyID2);
        if (object == nullptr) {
            continue;
        }
        if (!filter_accepts(object->identity, request.filter)) {
            ++stats.defensive_post_filter_rejections;
            continue;
        }
        const glm::vec3 point = from_jolt(result.mContactPointOn2);
        hits.push_back(CollisionHit{
            object->identity,
            glm::length(point - request.position),
            0.0f,
            point,
            from_jolt(-result.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisY())),
            result.mSubShapeID2.GetValue(),
        });
    }
    impl_->normalize(&hits);
    stats.final_hits_accepted = hits.size();
    impl_->record_stats(stats);
    return hits;
}

bool PhysicsWorld::ray_cast_closest(
    const RayCastRequest& request,
    CollisionHit* hit) const {
    const std::vector<CollisionHit> hits = ray_cast_all(request);
    if (hits.empty() || hit == nullptr) {
        return false;
    }
    *hit = hits.front();
    return true;
}

bool PhysicsWorld::shape_cast_closest(
    const ShapeCastRequest& request,
    CollisionHit* hit) const {
    std::shared_lock lock(impl_->mutex_);
    CollisionQueryStats stats{};
    stats.shape_cast_query_count = 1;
    if (!valid() || hit == nullptr) {
        impl_->record_stats(stats);
        return false;
    }
    JPH::RefConst<JPH::Shape> shape = make_shape(request.shape);
    if (shape == nullptr) {
        impl_->record_stats(stats);
        return false;
    }
    const glm::vec3 start = request.start + request.rotation * request.shape.local_center;
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        shape,
        JPH::Vec3::sReplicate(1.0f),
        JPH::RMat44::sRotationTranslation(
            to_jolt(request.rotation), to_jolt_r(start)),
        to_jolt(request.displacement));
    QueryBroadPhaseLayerFilter broadphase_filter(
        request.filter,
        impl_->unclassified_damageable_count_ != 0,
        impl_->local_stats(&stats));
    QueryObjectLayerFilter object_layer_filter(
        request.filter, impl_->local_stats(&stats));

    // Filters during collection rather than after it. The per-object half of the
    // filter -- ignored_entity_net_id, ignored_collider_id, gameplay_category --
    // is not expressible as a Jolt layer filter, so a stock
    // ClosestHitCollisionCollector would lock onto the nearest raw hit, and
    // report nothing at all when that hit is one the caller excluded. Rejecting
    // inside AddHit also means the early-out fraction only ever shrinks to hits
    // the caller would actually accept, which is where the saving comes from:
    // the cast stops reaching past the first real blocker.
    class ClosestAcceptedCollector final : public JPH::CastShapeCollector {
    public:
        ClosestAcceptedCollector(
            const Impl& impl,
            const CollisionQueryFilter& filter,
            CollisionQueryStats* stats)
            : impl_(impl), filter_(filter), stats_(stats) {}

        void AddHit(const JPH::ShapeCastResult& result) override {
            if (stats_ != nullptr) {
                ++stats_->raw_jolt_hits_collected;
            }
            if (result.mFraction >= GetEarlyOutFraction()) {
                return;
            }
            const Impl::StoredObject* object = impl_.find(result.mBodyID2);
            if (object == nullptr) {
                return;
            }
            if (!filter_accepts(object->identity, filter_)) {
                if (stats_ != nullptr) {
                    ++stats_->defensive_post_filter_rejections;
                }
                return;
            }
            best_ = result;
            best_identity_ = object->identity;
            has_hit_ = true;
            UpdateEarlyOutFraction(result.mFraction);
        }

        bool has_hit() const { return has_hit_; }
        const JPH::ShapeCastResult& best() const { return best_; }
        const CollisionObjectIdentity& best_identity() const {
            return best_identity_;
        }

    private:
        const Impl& impl_;
        const CollisionQueryFilter& filter_;
        CollisionQueryStats* stats_;
        JPH::ShapeCastResult best_{};
        CollisionObjectIdentity best_identity_{};
        bool has_hit_ = false;
    };

    ClosestAcceptedCollector collector(
        *impl_, request.filter, impl_->local_stats(&stats));
    impl_->system_->GetNarrowPhaseQuery().CastShape(
        cast,
        JPH::ShapeCastSettings{},
        JPH::RVec3::sZero(),
        collector,
        broadphase_filter,
        object_layer_filter);

    if (!collector.has_hit()) {
        impl_->record_stats(stats);
        return false;
    }
    const float cast_length = glm::length(request.displacement);
    const JPH::ShapeCastResult& result = collector.best();
    *hit = CollisionHit{
        collector.best_identity(),
        result.mFraction * cast_length,
        result.mFraction,
        from_jolt(result.mContactPointOn2),
        from_jolt(-result.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisY())),
        result.mSubShapeID2.GetValue(),
    };
    stats.final_hits_accepted = 1;
    impl_->record_stats(stats);
    return true;
}

}  // namespace network_example::physics
