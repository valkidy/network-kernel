// A scheduled hit (DamageRequest::apply_tick) lands on its tick and not
// before, takes no defensive grace on top, can still be dodged at its arrival,
// and can be cancelled while on its way. A hit on a dead target lands nothing.
//
// Every check uses require(), never assert(): -c opt compiles assert out along
// with the call inside it.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "simulation/public/simulation.h"
#include "world/public/world.h"

namespace {

namespace ne = network_example;

constexpr ne::PeerId kTargetPeer = 4;
constexpr std::uint64_t kTickUs = 33333;

void require(
    bool condition,
    const std::source_location& location = std::source_location::current()) {
    if (!condition) {
        std::fprintf(
            stderr,
            "require failed at %s:%u\n",
            location.file_name(),
            location.line());
        std::abort();
    }
}

ne::NetId spawn_enemy(ne::World& world) {
    const ne::NetId enemy = world.spawn_enemy(glm::vec3{0.0f});
    world.registry().get<ne::Health>(*world.find_entity(enemy)) =
        ne::Health{100, 100};
    return enemy;
}

ne::NetId spawn_player(ne::World& world) {
    const ne::NetId player = world.spawn_player(kTargetPeer, glm::vec3{0.0f});
    world.registry().get<ne::Health>(*world.find_entity(player)) =
        ne::Health{100, 100};
    return player;
}

std::uint16_t hp(ne::World& world, ne::NetId net_id) {
    return world.registry().get<ne::Health>(*world.find_entity(net_id)).hp;
}

// A missile from an agent (source peer 0), fired on tick 10 and landing on
// `apply_tick`. Its hit time is the arrival, as the weapon will write it.
ne::DamageRequest missile(ne::NetId target, std::uint32_t apply_tick) {
    ne::DamageRequest request = ne::damage_request_at(
        10,
        0,
        999,
        target,
        0,
        1,
        30,
        apply_tick * kTickUs,
        glm::vec3{0.0f});
    request.apply_tick = apply_tick;
    return request;
}

// Drains and applies what is ready at `tick`, the way the kernel does.
void confirm_at(ne::World& world, ne::DamagePipeline& pipeline, std::uint32_t tick) {
    std::vector<KernelEvent> events;
    pipeline.confirm_ready(world, tick * kTickUs, tick, &events);
}

void a_scheduled_hit_lands_on_its_tick_and_not_before() {
    ne::World world;
    const ne::NetId target = spawn_enemy(world);
    ne::DamagePipeline pipeline;
    require(pipeline.submit_damage_request(missile(target, 20)));

    // Far past the hit time in microseconds: the tick is what decides.
    std::vector<KernelEvent> events;
    pipeline.confirm_ready(world, 1000u * kTickUs, 19, &events);
    require(hp(world, target) == 100);
    require(pipeline.pending_count() == 1);

    confirm_at(world, pipeline, 20);
    require(hp(world, target) == 70);
    require(pipeline.pending_count() == 0);
}

// A hit from an agent on a player normally waits 100 ms for the player to
// dodge. A scheduled hit already arrives at its own time and takes none --
// the unscheduled control, submitted alongside, is still waiting.
void a_scheduled_hit_takes_no_defensive_grace() {
    ne::World world;
    const ne::NetId player = spawn_player(world);
    ne::DamagePipeline pipeline;
    require(pipeline.submit_damage_request(missile(player, 20)));
    ne::DamageRequest plain = missile(player, 0);
    plain.hit_time_us = 20 * kTickUs;
    plain.damage = 5;
    require(pipeline.submit_damage_request(plain));

    confirm_at(world, pipeline, 20);
    require(hp(world, player) == 70);
    require(pipeline.pending_count() == 1);
}

// The dodge window is judged against the arrival: a dodge pressed just before
// the missile lands cancels it, one pressed long before does not.
void a_dodge_at_the_arrival_cancels_the_hit() {
    for (const bool in_time : {true, false}) {
        ne::World world;
        const ne::NetId player = spawn_player(world);
        ne::DamagePipeline pipeline;
        require(pipeline.submit_damage_request(missile(player, 20)));
        KernelPlayerInput dodge{};
        dodge.buttons = InputButton_Dodge;
        const std::uint64_t pressed =
            in_time ? 20 * kTickUs - 50000u : 12 * kTickUs;
        pipeline.ingest_defensive_input(kTargetPeer, dodge, pressed, pressed, true);
        confirm_at(world, pipeline, 20);
        require(hp(world, player) == (in_time ? 100 : 70));
    }
}

void a_cancelled_hit_never_lands() {
    ne::World world;
    const ne::NetId target = spawn_enemy(world);
    const ne::NetId bystander = spawn_enemy(world);
    ne::DamagePipeline pipeline;
    require(pipeline.submit_damage_request(missile(target, 20)));
    require(pipeline.submit_damage_request(missile(bystander, 20)));
    // Once while still queued, and again for one already moved to pending.
    std::vector<KernelEvent> events;
    pipeline.confirm_ready(world, 15 * kTickUs, 15, &events);
    require(pipeline.submit_damage_request(missile(target, 21)));
    pipeline.cancel_pending_for_target(target);

    confirm_at(world, pipeline, 21);
    require(hp(world, target) == 100);
    require(hp(world, bystander) == 70);
}

// The killing blow and a second hit land in the same drain. The dead take the
// second as nothing: no damage, no hit event, no second death.
void a_hit_on_a_dead_target_lands_nothing() {
    ne::World world;
    const ne::NetId target = spawn_enemy(world);
    world.registry().get<ne::Health>(*world.find_entity(target)).hp = 0;
    const std::vector<ne::ConfirmedDamage> hits{
        ne::ConfirmedDamage{0, 0, 999, target, 0, 1, 30, 0, glm::vec3{0.0f}},
    };
    std::vector<KernelEvent> events;
    const std::vector<ne::ConfirmedDamage> depleted =
        ne::apply_damage_applications(world, hits, 0, &events);
    require(depleted.empty());
    require(events.empty());
    require(hp(world, target) == 0);
}

}  // namespace

int main() {
    a_scheduled_hit_lands_on_its_tick_and_not_before();
    a_scheduled_hit_takes_no_defensive_grace();
    a_dodge_at_the_arrival_cancels_the_hit();
    a_cancelled_hit_never_lands();
    a_hit_on_a_dead_target_lands_nothing();
    std::puts("scheduled_damage_test passed");
    return 0;
}
