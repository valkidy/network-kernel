// The world timeline -- remote actors, thrown props, the endings held back for
// them -- used to be drawn at "clock-sync estimate minus the interpolation
// delay", recomputed every frame and clamped to the newest snapshot. A stream
// running late therefore stopped everything at once and jumped it forward when
// the next snapshot came in (measured: a thrown bottle held at 24.0 m, then
// 27.2 m), and an offset correction that moved the estimate back ran the whole
// world backwards. These pin the render clock that replaced it: it runs at real
// time, bends toward its target by a tenth at most, may overrun the newest
// snapshot by a quarter second, and never runs back.
//
// Client-only setup, as in world_timeline_endings_test, but with clock sync:
// the offset is zero, so client time and server time are the same number, and
// snapshots arrive the instant their tick is reached unless a test stalls them.

// Every standard and third-party header goes in before the `private` define
// below, or it rewrites access specifiers inside libc++ instead of inside the
// kernel.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "protocol/public/network_packets.h"
#include "world/public/components.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace ne = network_example;

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint32_t kTrajectoryTemplateId = 21;
constexpr std::uint32_t kBlastTemplateId = 22;
constexpr std::uint32_t kBlastColliderTemplateId = 9;
constexpr std::uint32_t kPropTemplateId = 7;
constexpr ne::NetId kBottle = 51;
constexpr ne::NetId kBlast = 70;
constexpr std::uint32_t kThrowTick = 10;
constexpr float kBottleSpeedX = 12.0f;
constexpr std::uint64_t kFrameUs = 16667;
constexpr std::uint64_t kOverrunCapUs = 250000;

struct Client {
    Client() : engine(make_config()) {
        engine.reset_runtime_state(KernelMode_Client);
        engine.local_client_peer_id_ = 2;
        engine.has_client_clock_sync_ = true;
        engine.client_clock_offset_us_ = 0;

        ne::RuntimeProjectileTemplate trajectory{};
        trajectory.projectile_template_id = kTrajectoryTemplateId;
        trajectory.motion_model = ne::ProjectileMotionModel::kParabolic;
        trajectory.sync_mode = ne::ProjectileSyncMode::kLocalPredictedDeterministic;
        trajectory.speed = 24.0f;
        trajectory.gravity = glm::vec3{0.0f, -9.81f, 0.0f};
        engine.catalog_runtime_.projectile_templates.push_back(trajectory);

        ne::RuntimeProjectileTemplate blast{};
        blast.projectile_template_id = kBlastTemplateId;
        blast.collider_template_id = kBlastColliderTemplateId;
        blast.motion_model = ne::ProjectileMotionModel::kLinear;
        blast.sync_mode = ne::ProjectileSyncMode::kServerSnapshotOnly;
        engine.catalog_runtime_.projectile_templates.push_back(blast);

        KernelEntityTemplateDefinition prop_template{};
        prop_template.struct_size = sizeof(prop_template);
        prop_template.entity_template_id = kPropTemplateId;
        prop_template.entity_type = static_cast<std::uint16_t>(ne::EntityType::kProp);
        prop_template.prop.struct_size = sizeof(prop_template.prop);
        prop_template.prop.throw_trajectory_projectile_template_id =
            kTrajectoryTemplateId;
        engine.entity_templates_.push_back(prop_template);
    }

    static KernelConfig make_config() {
        KernelConfig config{};
        config.mode = KernelMode_Client;
        config.tick.server_tick_rate = 30;
        config.tick.snapshot_rate = 15;
        return config;
    }

    float dt() const { return engine.tick_loop_.fixed_delta_seconds(); }
    std::uint64_t tick_us(std::uint32_t tick) const {
        return static_cast<std::uint64_t>(
            static_cast<double>(tick) * static_cast<double>(dt()) * 1000000.0);
    }
    std::uint32_t snapshot_interval() const {
        return engine.tick_loop_.snapshot_interval_ticks();
    }

    // Every snapshot the server has published by `now`, unless stalled.
    void deliver_snapshots() {
        if (stalled) {
            return;
        }
        while (tick_us(next_snapshot_tick) <= now) {
            ne::WorldSnapshot snapshot;
            snapshot.header.server_tick = next_snapshot_tick;
            engine.handle_client_snapshot(snapshot);
            newest_tick = next_snapshot_tick;
            next_snapshot_tick += snapshot_interval();
        }
    }

    void throw_bottle() {
        ne::EntitySpawnPacket spawn{};
        spawn.net_id = kBottle;
        spawn.entity_type = ne::EntityType::kProp;
        spawn.server_tick = kThrowTick;
        spawn.position = glm::vec3{0.0f, 1.0f, 0.0f};
        spawn.entity_template_id = kPropTemplateId;
        spawn.item_template_id = 13;
        spawn.item_instance_id = 1001;
        spawn.world_item_mode = KernelWorldItemMode_InFlight;
        engine.handle_client_spawn(spawn);

        ne::PropStateChangeBatchPacket batch{};
        batch.server_tick = kThrowTick;
        ne::PropStateChangeRecord record{};
        record.net_id = kBottle;
        record.changed_fields = ne::kPropStateChangeMode |
            ne::kPropStateChangeTransform | ne::kPropStateChangeVelocity;
        record.world_mode = KernelWorldItemMode_InFlight;
        record.position = glm::vec3{0.0f, 1.0f, 0.0f};
        record.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        record.velocity = glm::vec3{kBottleSpeedX, 6.0f, 0.0f};
        batch.records.push_back(record);
        engine.handle_client_prop_state_change_batch(batch);
    }

    // Up to the throw, with the stream flowing and the clock settled.
    void start() {
        now = tick_us(kThrowTick);
        deliver_snapshots();
        throw_bottle();
        for (int frame = 0; frame < 30; ++frame) {
            this->frame();
        }
    }

    void spawn_blast(std::uint32_t tick) {
        ne::ProjectileSpawnBatchPacket packet{};
        packet.server_tick = tick;
        packet.catalog_hash = engine.catalog_hash_;
        ne::ProjectileSpawnGroup group{};
        group.projectile_template_id = kBlastTemplateId;
        ne::ProjectileSpawnRecord record{};
        record.projectile_net_id = kBlast;
        record.owner_peer = 0;
        record.spawn_position = glm::vec3{8.0f, 0.0f, 0.0f};
        group.records.push_back(record);
        packet.groups.push_back(group);
        engine.handle_client_projectile_spawn_batch(packet);
    }

    // One rendered frame: time advances, whatever has arrived is delivered,
    // and the world is drawn. Returns the instant it was drawn at.
    std::uint64_t frame() {
        now += kFrameUs;
        deliver_snapshots();
        count = engine.get_render_states_at_time(
            now, states.data(), static_cast<std::uint32_t>(states.size()));
        return engine.render_server_time_us_;
    }

    const RenderEntityState* drawn(ne::NetId net_id) const {
        for (std::uint32_t index = 0; index < count; ++index) {
            if (states[index].net_id == net_id) {
                return &states[index];
            }
        }
        return nullptr;
    }

    std::uint64_t target() const {
        return engine.render_target_server_time_us(now);
    }

    ne::KernelEngine engine;
    std::uint64_t now = 0;
    std::uint32_t next_snapshot_tick = 2;
    std::uint32_t newest_tick = 0;
    bool stalled = false;
    std::array<RenderEntityState, 8> states{};
    std::uint32_t count = 0;
};

// The stream stops at tick 40 for half a second. The bottle flies on past the
// newest snapshot to the overrun cap instead of stopping where it was, and when
// the stream comes back it is caught up gently: no frame moves it further than
// its own speed allows, give or take the catch-up rate.
void a_late_stream_does_not_stop_and_jump_the_world() {
    Client client;
    client.start();
    while (client.newest_tick < 40u) {
        client.frame();
    }
    client.stalled = true;
    const float max_step = kBottleSpeedX *
        (static_cast<float>(kFrameUs) / 1000000.0f) * 1.2f;

    const RenderEntityState* bottle = client.drawn(kBottle);
    require(bottle != nullptr);
    float previous_x = bottle->position.x;
    float furthest_while_stalled = previous_x;
    const std::uint64_t resume_at = client.tick_us(40) + 500000u;
    const std::uint64_t end_at = resume_at + 1000000u;
    while (client.now < end_at) {
        if (client.now >= resume_at) {
            client.stalled = false;
        }
        client.frame();
        bottle = client.drawn(kBottle);
        require(bottle != nullptr);
        require(bottle->position.x >= previous_x);
        require(bottle->position.x - previous_x <= max_step);
        if (client.stalled) {
            furthest_while_stalled = bottle->position.x;
        }
        previous_x = bottle->position.x;
    }
    // Past the newest snapshot by the whole cap, not stopped at it.
    const float at_newest = kBottleSpeedX *
        static_cast<float>(client.tick_us(40) - client.tick_us(kThrowTick)) /
        1000000.0f;
    const float cap_distance =
        kBottleSpeedX * static_cast<float>(kOverrunCapUs) / 1000000.0f;
    require(furthest_while_stalled >= at_newest + cap_distance * 0.95f);
    require(furthest_while_stalled <= at_newest + cap_distance * 1.01f);
    // And back on its target once the stream has been flowing a while.
    require(std::llabs(static_cast<long long>(client.engine.render_server_time_us_) -
                       static_cast<long long>(client.target())) <=
            static_cast<long long>(client.tick_us(1)));
}

// The clock-sync estimate is corrected 50 ms back. The world slows to 0.9x
// until it is back on target instead of running backwards.
void an_offset_correction_backwards_slows_the_world_down() {
    Client client;
    client.start();
    std::uint64_t previous = client.frame();
    client.engine.client_clock_offset_us_ = -50000;
    bool slowed = false;
    for (int frame = 0; frame < 60; ++frame) {
        const std::uint64_t drawn_at = client.frame();
        require(drawn_at >= previous);
        const std::uint64_t step = drawn_at - previous;
        // Never faster than real time while ahead of the target, and never
        // stopped outright.
        require(step <= kFrameUs + 1u);
        require(step >= kFrameUs * 85u / 100u);
        slowed = slowed || step < kFrameUs * 95u / 100u;
        previous = drawn_at;
    }
    require(slowed);
    // 50 ms at a tenth of real time is half a second, well inside 60 frames.
    require(std::llabs(static_cast<long long>(previous) -
                       static_cast<long long>(client.target())) <=
            static_cast<long long>(client.tick_us(1)));
}

// A three-second outage: the clock holds at the cap, and when the stream comes
// back it goes straight to its target -- forward, never back.
void a_long_outage_holds_at_the_cap_and_resumes_forward() {
    Client client;
    client.start();
    client.stalled = true;
    const std::uint32_t newest = client.newest_tick;
    const std::uint64_t resume_at = client.now + 3000000u;
    std::uint64_t previous = client.frame();
    while (client.now < resume_at) {
        const std::uint64_t drawn_at = client.frame();
        require(drawn_at >= previous);
        previous = drawn_at;
    }
    require(previous == client.tick_us(newest) + kOverrunCapUs);

    client.stalled = false;
    for (int frame = 0; frame < 10; ++frame) {
        const std::uint64_t drawn_at = client.frame();
        require(drawn_at >= previous);
        previous = drawn_at;
    }
    require(std::llabs(static_cast<long long>(previous) -
                       static_cast<long long>(client.target())) <=
            static_cast<long long>(client.tick_us(1)));
}

// While the clock overruns a stalled stream, the bottle's ending and the blast
// it set off still happen on the same frame: both read the one clock. The
// stream stalls at tick 40 and the despawn and the blast, both for tick 44,
// arrive on the reliable channel regardless.
void endings_stay_in_step_while_the_clock_overruns() {
    Client client;
    client.start();
    while (client.newest_tick < 40u) {
        client.frame();
    }
    client.stalled = true;
    client.spawn_blast(44u);
    ne::EntityDespawnPacket despawn{};
    despawn.net_id = kBottle;
    despawn.server_tick = 44u;
    despawn.reason = KernelDespawnReason_Destroyed;
    client.engine.handle_client_despawn(despawn);

    bool blast_seen = false;
    // Until the clock is held at the cap: tick 40 plus a quarter second, which
    // takes an interpolation delay longer than that in client time.
    const std::uint64_t cap = client.tick_us(40) + kOverrunCapUs;
    while (client.engine.render_server_time_us_ < cap) {
        client.frame();
        const bool bottle = client.drawn(kBottle) != nullptr;
        const bool blast = client.drawn(kBlast) != nullptr;
        require(bottle != blast);
        blast_seen = blast_seen || blast;
    }
    // Reached while the newest snapshot is still tick 40.
    require(client.newest_tick == 40u);
    require(blast_seen);
}

// An event held for the render instant -- a hit timed to the swing that dealt
// it -- is released on the same clock: while the stream is stalled at tick 40,
// one for tick 43 comes out when the world is drawn at tick 43, not when a
// snapshot for it finally arrives.
void held_events_are_released_while_the_clock_overruns() {
    Client client;
    client.start();
    while (client.newest_tick < 40u) {
        client.frame();
    }
    client.stalled = true;
    KernelEvent hit{};
    hit.type = KernelEventType_DamageApplied;
    hit.tick = 43u;
    hit.net_id = 99u;
    hit.presentation_time_us = client.tick_us(43u);
    client.engine.pending_presentation_events_.push_back(hit);

    const auto released = [&client]() {
        std::array<KernelEvent, 32> polled{};
        const std::uint32_t count = client.engine.poll_events(
            polled.data(), static_cast<std::uint32_t>(polled.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            if (polled[index].net_id == 99u) {
                return true;
            }
        }
        return false;
    };
    bool seen = false;
    const std::uint64_t cap = client.tick_us(40) + kOverrunCapUs;
    while (client.engine.render_server_time_us_ < cap) {
        client.frame();
        const bool now_released = released();
        require(!now_released ||
                client.engine.render_server_time_us_ >= client.tick_us(43u));
        seen = seen || now_released;
    }
    require(client.newest_tick == 40u);
    require(seen);
}

}  // namespace

int main() {
    a_late_stream_does_not_stop_and_jump_the_world();
    an_offset_correction_backwards_slows_the_world_down();
    a_long_outage_holds_at_the_cap_and_resumes_forward();
    endings_stay_in_step_while_the_clock_overruns();
    held_events_are_released_while_the_clock_overruns();
    std::printf("render_clock_test: PASS\n");
    return 0;
}
