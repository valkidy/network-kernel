#include "kernel/src/derived_chain_presenter.h"

#include <algorithm>
#include <utility>

#include "physics/public/physics_world.h"
#include "simulation/public/simulation.h"

namespace network_example {

DerivedChainPresenter::~DerivedChainPresenter() = default;

void DerivedChainPresenter::clear() {
    chains_.clear();
}

void DerivedChainPresenter::add_root(
    const DerivedChainRoot& root,
    const GameplayCatalogRuntime* catalog) {
    if (root.net_id == 0u || catalog == nullptr) {
        return;
    }
    for (const Chain& chain : chains_) {
        if (chain.root.net_id == root.net_id) {
            return;
        }
    }
    const RuntimeProjectileTemplate* root_template =
        catalog->find_projectile_template(root.projectile_template_id);
    if (root_template == nullptr || root_template->derived_chain_ticks == 0u) {
        return;
    }
    Chain chain;
    chain.root = root;
    // The server destroys a held root on spawn + lifetime + chain (its
    // lifetime ages from the spawn tick itself); one more tick lets the last
    // derived spawn of that tick be stepped too.
    chain.end_tick = root.spawn_tick + root_template->lifetime_ticks +
        root_template->derived_chain_ticks + 1u;
    // No standalone collision: the chain borrows the client's static world.
    chain.world = std::make_unique<World>(false, catalog);
    chains_.push_back(std::move(chain));
}

void DerivedChainPresenter::remove_root(NetId root_net_id) {
    chains_.erase(
        std::remove_if(
            chains_.begin(),
            chains_.end(),
            [root_net_id](const Chain& chain) {
                return chain.root.net_id == root_net_id;
            }),
        chains_.end());
}

std::uint32_t DerivedChainPresenter::natural_end_tick(NetId root_net_id) const {
    for (const Chain& chain : chains_) {
        if (chain.root.net_id == root_net_id) {
            return chain.end_tick - 1u;
        }
    }
    return 0u;
}

void DerivedChainPresenter::advance_to_tick(
    std::uint32_t tick,
    float fixed_delta_seconds,
    physics::PhysicsWorld* ground) {
    for (Chain& chain : chains_) {
        World& world = *chain.world;
        world.set_collision_world(ground);
        if (chain.next_tick == 0u) {
            if (tick < chain.root.spawn_tick) {
                continue;
            }
            const RuntimeProjectileTemplate* root_template =
                world.find_projectile_template(chain.root.projectile_template_id);
            if (root_template == nullptr) {
                chain.next_tick = chain.end_tick + 1u;
                continue;
            }
            // The root spawns the way the targeted strike spawned it on the
            // server: at its tick, with no launch salt, before that tick's
            // projectile pass ages it. The direction plays no part in any
            // derived pick, so any unit vector reproduces it.
            (void)spawn_projectile_at(
                world,
                *root_template,
                chain.root.owner_peer,
                chain.root.instigator,
                root_template->weapon_id,
                chain.root.action_instance_id,
                chain.root.position,
                glm::vec3{1.0f, 0.0f, 0.0f},
                chain.root.spawn_tick,
                fixed_delta_seconds,
                nullptr);
            auto view = world.registry().view<NetworkIdentity, ProjectileState>();
            for (const entt::entity entity : view) {
                chain.shadow_root = view.get<NetworkIdentity>(entity).net_id;
            }
            chain.next_tick = chain.root.spawn_tick;
        }
        std::vector<KernelEvent> events;
        DamagePipeline damage_pipeline;
        while (chain.next_tick <= tick && chain.next_tick <= chain.end_tick) {
            // The two passes the server runs after its weapons, in its order.
            simulate_projectiles(
                world, fixed_delta_seconds, chain.next_tick, &events);
            simulate_area_effects(
                world, chain.next_tick, &events, &damage_pipeline);
            events.clear();
            ++chain.next_tick;
        }
    }
    chains_.erase(
        std::remove_if(
            chains_.begin(),
            chains_.end(),
            [](const Chain& chain) { return chain.next_tick > chain.end_tick; }),
        chains_.end());
}

void DerivedChainPresenter::append_render_states(
    std::uint64_t render_server_time_us,
    float fixed_delta_seconds,
    const std::function<std::uint64_t()>& allocate_entity_id,
    std::vector<RenderEntityState>* out_states) {
    if (out_states == nullptr || fixed_delta_seconds <= 0.0f) {
        return;
    }
    const double render_seconds =
        static_cast<double>(render_server_time_us) / 1000000.0;
    for (Chain& chain : chains_) {
        if (chain.next_tick == 0u) {
            continue;
        }
        World& world = *chain.world;
        const std::uint32_t stepped_tick = chain.next_tick - 1u;
        // Only the slice of a tick since the last step is extrapolated; the
        // step itself is exact.
        const float since_step = static_cast<float>(std::clamp(
            render_seconds -
                static_cast<double>(stepped_tick) * fixed_delta_seconds,
            0.0,
            static_cast<double>(fixed_delta_seconds)));
        std::vector<std::pair<NetId, std::uint64_t>> alive_ids;
        auto view = world.registry()
                        .view<NetworkIdentity, Transform, Velocity, ProjectileState>();
        for (const entt::entity entity : view) {
            const NetId shadow_id = view.get<NetworkIdentity>(entity).net_id;
            if (shadow_id == chain.shadow_root) {
                continue;
            }
            const ProjectileState& projectile = view.get<ProjectileState>(entity);
            std::uint64_t entity_id = 0;
            for (const auto& known : chain.entity_ids) {
                if (known.first == shadow_id) {
                    entity_id = known.second;
                }
            }
            if (entity_id == 0u) {
                entity_id = allocate_entity_id();
            }
            alive_ids.emplace_back(shadow_id, entity_id);

            const glm::vec3 velocity = view.get<Velocity>(entity).linear;
            const RuntimeProjectileTemplate* projectile_template =
                world.find_projectile_template(projectile.projectile_template_id);
            RenderEntityState state{};
            state.entity_id = entity_id;
            state.net_id = 0u;
            state.entity_type = static_cast<std::uint16_t>(EntityType::kProjectile);
            state.owner_peer = chain.root.owner_peer;
            const glm::vec3 position =
                view.get<Transform>(entity).position + velocity * since_step;
            state.position = KernelVec3{position.x, position.y, position.z};
            state.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
            state.velocity = KernelVec3{velocity.x, velocity.y, velocity.z};
            state.spawn_tick = projectile.spawn_tick;
            state.action_instance_id = chain.root.action_instance_id;
            // Exact, not a guess: the same simulation the server ran.
            state.status = RenderEntityStatus_Active;
            state.template_id = projectile.projectile_template_id;
            state.collider_template_id = projectile_template != nullptr
                ? projectile_template->collider_template_id
                : 0u;
            state.aim_direction = KernelVec3{1.0f, 0.0f, 0.0f};
            out_states->push_back(state);
        }
        chain.entity_ids = std::move(alive_ids);
    }
}

}  // namespace network_example
