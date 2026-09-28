#ifndef KERNEL_SRC_DERIVED_CHAIN_PRESENTER_H_
#define KERNEL_SRC_DERIVED_CHAIN_PRESENTER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "kernel/public/kernel_types.h"
#include "world/public/components.h"
#include "world/public/world.h"

namespace network_example {

namespace physics {
class PhysicsWorld;
}

// What a client was sent about a derived chain's root: its spawn record.
struct DerivedChainRoot {
    NetId net_id = 0;
    std::uint32_t projectile_template_id = 0;
    PeerId owner_peer = 0;
    NetId instigator = 0;
    std::uint32_t action_instance_id = 0;
    glm::vec3 position{0.0f};
    std::uint32_t spawn_tick = 0;
};

// Draws the projectiles a server never sends.
//
// A derived chain descends from a replicated root through spawns whose every
// choice is seeded from what the root's spawn record carries. So rather than
// being told about them, a client re-runs them: each root gets a World of its
// own, fed the same catalog and the static world, and the same projectile and
// area-effect passes the server runs are stepped over it tick by tick. It has
// no actors, so it decides nothing about damage -- that still arrives in
// snapshots. What it produces is where every fuse, meteor and blast is, and
// when.
class DerivedChainPresenter {
public:
    DerivedChainPresenter() = default;
    ~DerivedChainPresenter();

    // Forgets every chain. Call when the catalog or the session changes.
    void clear();

    // Starts deriving the chain under `root`, unless it already is. The chain
    // is simulated from the root's spawn tick on the next advance, however
    // late that is: a root handed over mid-chain is fast-forwarded.
    void add_root(
        const DerivedChainRoot& root,
        const GameplayCatalogRuntime* catalog);

    // A root gone before its chain could have ended: the server called it
    // off, or it left this client's range. Its chain is dropped.
    void remove_root(NetId root_net_id);

    // The tick a root's chain naturally ends on -- when the server destroys
    // the held root. Zero for a root this presenter does not hold.
    std::uint32_t natural_end_tick(NetId root_net_id) const;

    // Steps every chain up to and including `tick`, and drops those that
    // have ended.
    void advance_to_tick(
        std::uint32_t tick,
        float fixed_delta_seconds,
        physics::PhysicsWorld* ground);

    // Appends one render state per derived projectile alive at
    // `render_server_time_us`, placed by extrapolating from the last stepped
    // tick. The roots themselves are not included: they are replicated and
    // drawn as such. `allocate_entity_id` gives a new client-local id.
    void append_render_states(
        std::uint64_t render_server_time_us,
        float fixed_delta_seconds,
        const std::function<std::uint64_t()>& allocate_entity_id,
        std::vector<RenderEntityState>* out_states);

    std::size_t chain_count() const { return chains_.size(); }

private:
    struct Chain {
        DerivedChainRoot root;
        std::uint32_t end_tick = 0;
        std::uint32_t next_tick = 0;
        NetId shadow_root = 0;
        std::unique_ptr<World> world;
        // Shadow net id -> stable client entity id.
        std::vector<std::pair<NetId, std::uint64_t>> entity_ids;
    };

    std::vector<Chain> chains_;
};

}  // namespace network_example

#endif  // KERNEL_SRC_DERIVED_CHAIN_PRESENTER_H_
