// A snapshot is a send set, and past the budget most actors sit out most of
// them: measured at 80 acting agents, half of every agent's snapshot intervals
// even inside 10 m. The client used to pair the two snapshots around the
// render instant and hold any actor the later one lacked -- then, once neither
// had it, draw it at the newest sample it had received, a full interpolation
// delay ahead -- then snap back to the later snapshot. These pin the
// replacement: each actor is drawn from its own samples.
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#define private public
#include "kernel/src/kernel.h"
#undef private

#include "sync/public/snapshot.h"
#include "world/public/components.h"

namespace ne = network_example;

namespace {

// Not assert(): -c opt defines NDEBUG and would compile every check out,
// including any call inside one.
void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kEpsilon = 0.001f;
constexpr float kRunSpeed = 3.0f;

ne::EntitySnapshot actor(ne::NetId net_id, float x) {
    ne::EntitySnapshot entity;
    entity.net_id = net_id;
    entity.type = ne::EntityType::kActor;
    entity.actor_type = ne::ActorType::kAgent;
    entity.position = glm::vec3{x, 1.0f, 0.0f};
    // The vertical component is what a grounded controller presses into the
    // floor with; stepping along it would sink the body.
    entity.velocity = glm::vec3{kRunSpeed, -5.0f, 0.0f};
    entity.hp = 100;
    return entity;
}

struct Harness {
    Harness() : engine(make_config()) {}

    static KernelConfig make_config() {
        KernelConfig config{};
        config.mode = KernelMode_Client;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        return config;
    }

    void store(std::uint32_t tick, std::vector<ne::EntitySnapshot> entities) {
        ne::WorldSnapshot snapshot;
        snapshot.header.server_tick = tick;
        snapshot.entities = std::move(entities);
        engine.store_client_snapshot(std::move(snapshot));
    }

    // Same conversion as kernel.cc's tick_time_us.
    std::uint64_t time_us(double tick) const {
        return static_cast<std::uint64_t>(
            tick * static_cast<double>(engine.tick_loop_.fixed_delta_seconds()) *
            1000000.0);
    }

    ne::WorldSnapshot at(double tick) const {
        ne::WorldSnapshot out;
        const bool built =
            engine.build_interpolated_snapshot_for_server_time(time_us(tick), &out);
        require(built);
        return out;
    }

    ne::KernelEngine engine;
};

const ne::EntitySnapshot* find(const ne::WorldSnapshot& snapshot, ne::NetId net_id) {
    for (const ne::EntitySnapshot& entity : snapshot.entities) {
        if (entity.net_id == net_id) {
            return &entity;
        }
    }
    return nullptr;
}

float x_at(const ne::WorldSnapshot& snapshot, ne::NetId net_id) {
    const ne::EntitySnapshot* entity = find(snapshot, net_id);
    require(entity != nullptr);
    return entity->position.x;
}

bool near(float lhs, float rhs) {
    return std::fabs(lhs - rhs) < kEpsilon;
}

float seconds(double ticks) {
    return static_cast<float>(ticks / 30.0);
}

// Seen at 100 and 106, skipped at 102 and 104. Between the two it is drawn on
// the line joining them, not held at 100.
void a_skipped_actor_is_interpolated_across_the_gap() {
    Harness h;
    const float at_106 = kRunSpeed * seconds(6);
    h.store(100, {actor(10, 0.0f)});
    h.store(102, {});
    h.store(104, {});
    h.store(106, {actor(10, at_106)});
    h.store(108, {});

    const float x = x_at(h.at(102), 10);
    std::printf("gap bridged: x=%.4f expected=%.4f\n", x, at_106 / 3.0f);
    require(near(x, at_106 / 3.0f));
    // Neither 104 nor 106 bracket it as a pair either -- it used to leave the
    // interpolated snapshot altogether here.
    require(near(x_at(h.at(105), 10), at_106 * 5.0f / 6.0f));
}

// No newer sample yet: carried along its last horizontal velocity, never along
// the vertical one, and only up to the cap.
void a_starved_actor_is_extrapolated_horizontally_up_to_the_cap() {
    Harness h;
    h.store(100, {actor(11, 0.0f)});
    h.store(102, {});
    h.store(104, {});
    h.store(106, {});
    h.store(108, {});

    const ne::WorldSnapshot mid = h.at(104);
    const ne::EntitySnapshot* entity = find(mid, 11);
    require(entity != nullptr);
    std::printf("extrapolated: x=%.4f y=%.4f\n", entity->position.x, entity->position.y);
    require(near(entity->position.x, kRunSpeed * seconds(4)));
    require(near(entity->position.y, 1.0f));

    // Eight ticks is 0.267 s, past the quarter-second cap. 108 is also the
    // newest snapshot, the early-return path.
    require(near(x_at(h.at(108), 11), kRunSpeed * 0.25f));
}

// Flags, hp and the action timeline switch no earlier than they always have:
// the last interval before the sample that carries them.
void discrete_state_waits_for_the_last_interval() {
    Harness h;
    ne::EntitySnapshot hurt = actor(12, kRunSpeed * seconds(8));
    hurt.hp = 40;
    h.store(100, {actor(12, 0.0f)});
    h.store(102, {});
    h.store(104, {});
    h.store(106, {});
    h.store(108, {hurt});

    const ne::WorldSnapshot early_snapshot = h.at(105);
    const ne::EntitySnapshot* early = find(early_snapshot, 12);
    require(early != nullptr);
    require(early->hp == 100);
    require(near(early->position.x, kRunSpeed * seconds(5)));
    const ne::WorldSnapshot late_snapshot = h.at(107);
    const ne::EntitySnapshot* late = find(late_snapshot, 12);
    require(late != nullptr);
    require(late->hp == 40);
}

// A dead actor stays where it fell, and a revive is placed rather than walked.
void the_dead_are_neither_extrapolated_nor_slid_out_of_the_corpse() {
    Harness h;
    ne::EntitySnapshot corpse = actor(13, 0.0f);
    corpse.flags |= ne::kVisualFlagDead;
    h.store(100, {corpse});
    h.store(102, {});
    h.store(104, {});
    h.store(106, {});
    h.store(108, {actor(13, 5.0f)});

    const ne::WorldSnapshot held_snapshot = h.at(104);
    const ne::EntitySnapshot* held = find(held_snapshot, 13);
    require(held != nullptr);
    require(near(held->position.x, 0.0f));
    require((held->flags & ne::kVisualFlagDead) != 0u);
    const ne::WorldSnapshot revived_snapshot = h.at(107);
    const ne::EntitySnapshot* revived = find(revived_snapshot, 13);
    require(revived != nullptr);
    require(near(revived->position.x, 5.0f));
    require((revived->flags & ne::kVisualFlagDead) == 0u);
}

// Scope: a prop keeps the pairwise hold. Its moves are pickups and placements,
// which interpolating across a gap would draw as a slide.
void a_prop_keeps_the_pairwise_result() {
    Harness h;
    ne::EntitySnapshot prop = actor(20, 0.0f);
    prop.type = ne::EntityType::kProp;
    prop.actor_type = ne::ActorType::kUnknown;
    ne::EntitySnapshot moved = prop;
    moved.position.x = 5.0f;
    h.store(100, {prop});
    h.store(102, {});
    h.store(104, {moved});

    require(near(x_at(h.at(101), 20), 0.0f));
}

}  // namespace

int main() {
    a_skipped_actor_is_interpolated_across_the_gap();
    a_starved_actor_is_extrapolated_horizontally_up_to_the_cap();
    discrete_state_waits_for_the_last_interval();
    the_dead_are_neither_extrapolated_nor_slid_out_of_the_corpse();
    a_prop_keeps_the_pairwise_result();
    std::printf("remote_actor_gap_bridging_test: PASS\n");
    return 0;
}
