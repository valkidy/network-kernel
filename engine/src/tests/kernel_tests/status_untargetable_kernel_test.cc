// apply_untargetable on the kernel's own world: the physics world the
// authority's queries ask (fed by push_collider_into_physics, not the
// standalone world the simulation tests use), a thrown prop's on_collision --
// the water balloon's shape -- and agent vision. Every case pairs a held
// target with one that is not.
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
#include "simulation/src/systems.h"

namespace {

using namespace network_example;

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint32_t kPropCollider = 10u;
constexpr std::uint32_t kActorCollider = 20u;
constexpr std::uint32_t kVisionCone = 12u;
constexpr std::uint32_t kBalloonTemplate = 201u;

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
    collider.shape_params = KernelVec4{8.0f, 90.0f, 0.0f, 0.0f};
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

    void hold(NetId net_id) {
        StatusEffectState& state = engine.world_.registry().get_or_emplace<StatusEffectState>(
            *engine.world_.find_entity(net_id));
        ActiveStatusEffect active;
        active.instance_id = 1u;
        active.status_effect_id = 1401u;
        active.channel_id = 41u;
        active.expire_tick = 100000u;
        active.untargetable = true;
        state.active.push_back(active);
        engine.sync_entity_colliders_from_world();
    }

    void release(NetId net_id) {
        engine.world_.registry()
            .get<StatusEffectState>(*engine.world_.find_entity(net_id))
            .active.clear();
        engine.sync_entity_colliders_from_world();
    }

    NetId ray_hit(const glm::vec3& origin) const {
        physics::RayCastRequest request{};
        request.origin = origin;
        request.direction = glm::vec3{1.0f, 0.0f, 0.0f};
        request.max_distance = 20.0f;
        physics::CollisionHit hit{};
        return engine.physics_world_->ray_cast_closest(request, &hit)
            ? hit.identity.entity_net_id
            : 0u;
    }

    KernelEngine engine;
};

void the_authoritys_physics_world_leaves_its_hit_volume_out() {
    Fixture f;
    const NetId target = f.actor(KernelActorType_Player, KernelVec3{5.0f, 0.0f, 0.0f});
    require(f.ray_hit(glm::vec3{0.0f, 0.5f, 0.0f}) == target);
    f.hold(target);
    require(f.ray_hit(glm::vec3{0.0f, 0.5f, 0.0f}) == 0u);
    f.release(target);
    require(f.ray_hit(glm::vec3{0.0f, 0.5f, 0.0f}) == target);
}

void a_thrown_prop_does_not_strike_it() {
    for (const bool held : {false, true}) {
        Fixture f;
        KernelEntityTemplateDefinition balloon{};
        balloon.struct_size = sizeof(balloon);
        balloon.entity_template_id = kBalloonTemplate;
        balloon.entity_type = KernelEntityType_Prop;
        balloon.component_flags = KERNEL_ENTITY_COMPONENT_TRANSFORM |
            KERNEL_ENTITY_COMPONENT_VELOCITY | KERNEL_ENTITY_COMPONENT_HITBOX;
        balloon.collider_template_id = kPropCollider;
        balloon.ai.struct_size = sizeof(balloon.ai);
        balloon.movement.struct_size = sizeof(balloon.movement);
        balloon.collision_trigger.struct_size = sizeof(balloon.collision_trigger);
        balloon.collision_trigger_mask = KERNEL_COLLISION_MASK_ACTOR;
        balloon.collision_trigger.action_type = KernelEntityTriggerActionType_ApplyDamage;
        balloon.collision_trigger.target_source = KernelEntityRefSource_EventTarget;
        balloon.collision_trigger.damage_amount = 25;
        f.engine.entity_templates_.push_back(balloon);

        KernelServerEntityCreateInfo info{};
        info.struct_size = sizeof(info);
        info.entity_template_id = kBalloonTemplate;
        info.position = KernelVec3{0.0f, 0.0f, 0.0f};
        info.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
        std::uint32_t prop = 0;
        require(f.engine.server_create_entity(info, &prop));
        f.engine.world_.registry().replace<PropWorldMode>(
            *f.engine.world_.find_entity(prop), PropWorldMode{PropMode::kInFlight});

        const NetId target = f.actor(KernelActorType_Player, KernelVec3{0.0f, 0.0f, 0.0f});
        if (held) {
            f.hold(target);
        }
        CollisionTriggerSystem{}.update(f.engine, 1000);
        require(f.engine.damage_pipeline_.pending_count() == (held ? 0u : 1u));
    }
}

void an_agent_does_not_see_it() {
    for (const bool held : {false, true}) {
        Fixture f;
        const NetId agent = f.actor(KernelActorType_Agent, KernelVec3{0.0f, 0.0f, 0.0f});
        const NetId target = f.actor(KernelActorType_Player, KernelVec3{4.0f, 0.0f, 0.0f});
        KernelAgentVisionConfig looking{};
        looking.struct_size = sizeof(looking);
        looking.camp = KernelAgentCamp_EnemySide;
        looking.vision_collider_template_id = kVisionCone;
        looking.max_visible_hostiles = 4;
        looking.local_origin = KernelVec3{0.0f, 1.5f, 0.0f};
        looking.local_forward = KernelVec3{1.0f, 0.0f, 0.0f};
        f.engine.vision_configs_[agent] = looking;
        // Only an entity with a vision config is anyone's candidate.
        KernelAgentVisionConfig seen = looking;
        seen.camp = KernelAgentCamp_PlayerSide;
        seen.local_forward = KernelVec3{-1.0f, 0.0f, 0.0f};
        f.engine.vision_configs_[target] = seen;
        if (held) {
            f.hold(target);
        }
        f.engine.update_vision_states(1.0f / 30.0f);
        const auto state = f.engine.vision_states_.find(agent);
        require(state != f.engine.vision_states_.end());
        const KernelVisionStateView& view = state->second.view;
        const bool sees = std::find(
                              view.visible_hostiles,
                              view.visible_hostiles + view.visible_hostile_count,
                              target) != view.visible_hostiles + view.visible_hostile_count;
        if (sees == held) {
            std::fprintf(stderr, "held=%d hostiles=%u\n", held ? 1 : 0,
                         view.visible_hostile_count);
        }
        require(sees == !held);
    }
}

}  // namespace

int main() {
    the_authoritys_physics_world_leaves_its_hit_volume_out();
    a_thrown_prop_does_not_strike_it();
    an_agent_does_not_see_it();
    std::puts("status_untargetable_kernel_test passed");
    return 0;
}
