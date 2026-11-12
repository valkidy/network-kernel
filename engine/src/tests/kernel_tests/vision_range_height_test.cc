// What an agent's vision cone reaches, in height.
//
// The range test used to flatten the offset to the floor, so a player revived
// 50 m above a gingerbread (a revive lifts the body that far) was "seen" from
// 2 m away and chased round underneath it. The range now counts height; the
// angle stays horizontal, because a flyer's cone points level and has to keep
// seeing the ground it hovers over (drone 9 m up with a 20 m cone, airship 14 m
// with 35 m).
//
// Height is measured feet to feet, not from the cone's eye: an eye at 1.5 m
// must not shave the reach to a target standing on the same floor.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#define private public
#include "kernel/src/kernel.h"
#undef private
#include "physics/public/physics_world.h"

namespace {

using namespace network_example;

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "vision_range_height_test:%d: %s\n", line, expression);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint32_t kPropCollider = 10u;
constexpr std::uint32_t kActorCollider = 20u;
constexpr std::uint32_t kVisionCone = 12u;
// The cone's range in shape_params.x.
constexpr float kRange = 8.0f;

KernelColliderTemplateDefinition box_collider(std::uint32_t id, std::uint32_t layer_mask) {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = id;
    collider.shape_type = KernelColliderShapeType_Aabb;
    collider.center = KernelVec3{0.0f, 0.5f, 0.0f};
    collider.shape_params = KernelVec4{0.5f, 0.5f, 0.5f, 0.0f};
    collider.purpose_flags = KernelColliderPurpose_Hit;
    collider.layer_mask = layer_mask;
    return collider;
}

KernelColliderTemplateDefinition vision_cone() {
    KernelColliderTemplateDefinition collider{};
    collider.struct_size = sizeof(collider);
    collider.template_id = kVisionCone;
    collider.shape_type = KernelColliderShapeType_Cone;
    collider.center = KernelVec3{0.0f, 1.5f, 0.0f};
    collider.shape_params = KernelVec4{kRange, 90.0f, 0.0f, 0.0f};
    collider.layer_mask = KERNEL_COLLISION_LAYER_AGENT_VISION;
    collider.purpose_flags = KernelColliderPurpose_Vision;
    return collider;
}

struct Fixture {
    Fixture() : engine(config()) {
        engine.reset_runtime_state(KernelMode_DedicatedServer);
        if (engine.physics_world_ == nullptr) {
            engine.physics_world_ = std::make_unique<physics::PhysicsWorld>();
            engine.world_.set_collision_world(engine.physics_world_.get());
        }
        engine.collider_templates_.push_back(
            box_collider(kPropCollider, KERNEL_COLLISION_LAYER_NEUTRAL));
        engine.collider_templates_.push_back(
            box_collider(kActorCollider, KERNEL_COLLISION_LAYER_PLAYER_SIDE));
        engine.collider_templates_.push_back(vision_cone());
    }

    static KernelConfig config() {
        KernelConfig config{};
        config.mode = KernelMode_DedicatedServer;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        return config;
    }

    NetId actor(KernelActorType type, const KernelVec3& position) {
        KernelServerEntityCreateInfo info{};
        info.struct_size = sizeof(info);
        info.entity_type = KernelEntityType_Actor;
        info.actor_type = type;
        info.position = position;
        info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t net_id = 0;
        require(engine.server_create_entity(info, &net_id));
        const entt::entity entity = *engine.world_.find_entity(net_id);
        engine.world_.registry().replace<Health>(entity, Health{100, 100});
        engine.world_.registry().replace<Hitbox>(
            entity, Hitbox{{0.0f, 0.5f, 0.0f}, {0.5f, 0.5f, 0.5f}, kActorCollider});
        engine.materialize_entity_collider(net_id);
        engine.sync_entity_colliders_from_world();
        return net_id;
    }

    KernelEngine engine;
};

// Whether an agent with its feet at `agent_feet`, looking +x, sees a player whose feet are
// at `target_feet`.
bool sees(const KernelVec3& agent_feet, const KernelVec3& target_feet) {
    Fixture f;
    const NetId agent = f.actor(KernelActorType_Agent, agent_feet);
    const NetId target = f.actor(KernelActorType_Player, target_feet);
    KernelAgentVisionConfig looking{};
    looking.struct_size = sizeof(looking);
    looking.camp = KernelAgentCamp_EnemySide;
    looking.vision_collider_template_id = kVisionCone;
    looking.max_visible_hostiles = 4;
    looking.local_origin = KernelVec3{0.0f, 1.5f, 0.0f};
    looking.local_forward = KernelVec3{1.0f, 0.0f, 0.0f};
    f.engine.vision_configs_[agent] = looking;
    KernelAgentVisionConfig seen = looking;
    seen.camp = KernelAgentCamp_PlayerSide;
    seen.local_forward = KernelVec3{-1.0f, 0.0f, 0.0f};
    f.engine.vision_configs_[target] = seen;
    f.engine.update_vision_states(1.0f / 30.0f);
    const auto state = f.engine.vision_states_.find(agent);
    require(state != f.engine.vision_states_.end());
    const KernelVisionStateView& view = state->second.view;
    const bool listed = std::find(
                            view.visible_hostiles,
                            view.visible_hostiles + view.visible_hostile_count,
                            target) != view.visible_hostiles + view.visible_hostile_count;
    // The two answers have to agree: the AI reads the candidate, and focus
    // on a locked target reads the list.
    require(listed == (view.current_target_candidate == target));
    return listed;
}

}  // namespace

int main() {
    // The control: same floor, inside the range, ahead.
    require(sees(KernelVec3{0.0f, 0.0f, 0.0f}, KernelVec3{4.0f, 0.0f, 0.0f}));
    // Same floor, just inside the range's edge: the eye's own height does
    // not count against it.
    require(sees(KernelVec3{0.0f, 0.0f, 0.0f}, KernelVec3{kRange - 0.1f, 0.0f, 0.0f}));
    // A revived player 50 m overhead, 2 m out: out of reach.
    require(!sees(KernelVec3{0.0f, 0.0f, 0.0f}, KernelVec3{2.0f, 50.0f, 0.0f}));
    // Just past the range in three dimensions though inside it on the floor:
    // 4 out and 7 up is 8.06 m.
    require(!sees(KernelVec3{0.0f, 0.0f, 0.0f}, KernelVec3{4.0f, 7.0f, 0.0f}));
    // Just inside it: 4 out and 6 up is 7.2 m.
    require(sees(KernelVec3{0.0f, 0.0f, 0.0f}, KernelVec3{4.0f, 6.0f, 0.0f}));
    // A flyer 6 m up still sees the ground ahead of it within the range:
    // the angle stays horizontal, so steeply below is not out of the cone.
    require(sees(KernelVec3{0.0f, 6.0f, 0.0f}, KernelVec3{1.0f, 0.0f, 0.0f}));
    require(sees(KernelVec3{0.0f, 6.0f, 0.0f}, KernelVec3{5.0f, 0.0f, 0.0f}));
    require(!sees(KernelVec3{0.0f, 6.0f, 0.0f}, KernelVec3{5.5f, 0.0f, 0.0f}));
    std::puts("vision_range_height_test: ok");
    return 0;
}
