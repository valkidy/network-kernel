#include "physics/public/physics_world.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
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
constexpr JPH::BroadPhaseLayer kStaticWorldBroadPhaseLayer(0);
constexpr JPH::BroadPhaseLayer kDamageableActorBroadPhaseLayer(1);
constexpr JPH::BroadPhaseLayer kOtherMovingBroadPhaseLayer(2);

std::uint64_t jolt_version_id() {
    using JPH::uint64;
    return JPH_VERSION_ID;
}

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    std::uint32_t GetNumBroadPhaseLayers() const override { return 3; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        if (layer == kTerrainObjectLayer) {
            return kStaticWorldBroadPhaseLayer;
        }
        if (layer == kDamageablePlayerObjectLayer ||
            layer == kDamageableHostileObjectLayer ||
            layer == kDamageableNeutralObjectLayer) {
            return kDamageableActorBroadPhaseLayer;
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
            accepted = accepts_static || accepts_unclassified;
        }
        if (stats_ != nullptr && accepted) {
            ++stats_->broadphase_layers_accepted;
            if (layer == kDamageableActorBroadPhaseLayer) {
                ++stats_->damageable_actor_broadphase_layers_accepted;
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
        }
        if (stats_ != nullptr && accepted) {
            ++stats_->object_layers_accepted;
            if (layer == kDamageablePlayerObjectLayer) {
                ++stats_->player_object_layers_accepted;
            } else if (layer == kDamageableHostileObjectLayer) {
                ++stats_->hostile_object_layers_accepted;
            } else if (layer == kDamageableNeutralObjectLayer) {
                ++stats_->neutral_object_layers_accepted;
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
    return std::isfinite(shape.half_extents.x) &&
           std::isfinite(shape.half_extents.y) &&
           std::isfinite(shape.half_extents.z) &&
           shape.half_extents.x > 0.0f &&
           shape.half_extents.y > 0.0f &&
           shape.half_extents.z > 0.0f;
}

JPH::RefConst<JPH::Shape> make_shape(const CollisionShapeDescriptor& shape) {
    if (!valid_shape(shape)) {
        return nullptr;
    }
    if (shape.type == CollisionShapeType::kSphere) {
        return new JPH::SphereShape(shape.radius);
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
    return (filter.collision_mask & collision_layer_bit(identity.layer)) != 0 &&
           (filter.object_kind_mask & kind_bit) != 0 &&
           (identity.gameplay_category == 0 ||
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
    };

    bool valid() const { return system_ != nullptr; }

    bool add_body(
        const JPH::Shape* shape,
        const CollisionObjectIdentity& identity,
        const glm::vec3& position,
        const glm::quat& rotation,
        JPH::EMotionType motion_type,
        JPH::ObjectLayer object_layer,
        std::string* error) {
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
        objects_by_body_.emplace(
            body_key(body_id), StoredObject{identity, object_layer});
        if (object_layer == kDamageableUnclassifiedObjectLayer) {
            ++unclassified_damageable_count_;
        }
        return true;
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
        query_stats_.object_layer_filter_checks +=
            delta.object_layer_filter_checks;
        query_stats_.object_layers_accepted += delta.object_layers_accepted;
        query_stats_.player_object_layers_accepted +=
            delta.player_object_layers_accepted;
        query_stats_.hostile_object_layers_accepted +=
            delta.hostile_object_layers_accepted;
        query_stats_.neutral_object_layers_accepted +=
            delta.neutral_object_layers_accepted;
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
    std::unordered_map<std::uint32_t, JPH::BodyID> bodies_by_collider_;
    std::unordered_map<std::uint32_t, StoredObject> objects_by_body_;
    std::size_t unclassified_damageable_count_ = 0;
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
    JPH::RefConst<JPH::Shape> shape = make_shape(object.shape);
    if (shape == nullptr) {
        *error = "invalid collision shape dimensions";
        return false;
    }
    const glm::vec3 world_position =
        object.position + object.rotation * object.shape.local_center;
    if (!impl_->add_body(
            shape,
            object.identity,
            world_position,
            object.rotation,
            JPH::EMotionType::Kinematic,
            object_layer_for(object.identity),
            error)) {
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
    } else if (!enabled && bodies.IsAdded(found->second)) {
        bodies.RemoveBody(found->second);
    }
    return true;
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
        hits.push_back(CollisionHit{
            object->identity,
            result.mFraction * request.max_distance,
            result.mFraction,
            request.origin + displacement * result.mFraction,
            -request.direction / direction_length,
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
    const std::vector<CollisionHit> hits = shape_cast_all(request);
    if (hits.empty() || hit == nullptr) {
        return false;
    }
    *hit = hits.front();
    return true;
}

}  // namespace network_example::physics
