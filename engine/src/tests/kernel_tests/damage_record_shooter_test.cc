// Kernel_ServerQueryDamageRecords names who fired a hit, not what dealt it
// (AI plan K1). A projectile is destroyed on impact, before its damage is
// applied, so the kernel remembers each projectile's and beam's shooter while
// they exist and resolves a hit's source through that afterwards. Driven on the
// engine directly, as command_queue_test does: staging a real projectile hit
// on a player needs a session's rewind history this test would only fake.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <entt/entt.hpp>

#include "kernel/public/kernel_api.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
        std::abort();
    }
}
#define require(condition) require_impl(static_cast<bool>(condition), #condition, __LINE__)

KernelConfig server_config() {
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    return config;
}

entt::entity make(entt::registry& registry, network_example::NetId net_id) {
    const entt::entity entity = registry.create();
    network_example::NetworkIdentity identity{};
    identity.net_id = net_id;
    registry.emplace<network_example::NetworkIdentity>(entity, identity);
    return entity;
}

}  // namespace

int main() {
    network_example::KernelEngine engine(server_config());
    engine.reset_runtime_state(KernelMode_DedicatedServer);
    entt::registry& registry = engine.world_.registry();

    constexpr network_example::NetId kShooter = 11;
    constexpr network_example::NetId kProjectile = 21;
    constexpr network_example::NetId kBeam = 22;
    constexpr network_example::NetId kBlast = 23;  // fired by nobody
    constexpr network_example::NetId kSwordsman = 31;

    const entt::entity projectile = make(registry, kProjectile);
    network_example::ProjectileState shot{};
    shot.shooter_net_id = kShooter;
    registry.emplace<network_example::ProjectileState>(projectile, shot);
    const entt::entity beam = make(registry, kBeam);
    network_example::ProjectileBeamRuntime ray{};
    ray.shooter_net_id = kShooter;
    registry.emplace<network_example::ProjectileBeamRuntime>(beam, ray);
    const entt::entity blast = make(registry, kBlast);
    registry.emplace<network_example::ProjectileState>(blast, network_example::ProjectileState{});

    engine.remember_damage_source_shooters();
    // Gone, as a projectile is by the time its hit is applied.
    registry.destroy(projectile);
    registry.destroy(beam);
    registry.destroy(blast);

    require(engine.attacker_of(kProjectile) == kShooter);
    require(engine.attacker_of(kBeam) == kShooter);
    // A blast nobody fired stays credited to nobody.
    require(engine.attacker_of(kBlast) == 0u);
    // A melee or hitscan source is the actor itself.
    require(engine.attacker_of(kSwordsman) == kSwordsman);

    // And the record carries the resolved id.
    network_example::ConfirmedDamage hit{};
    hit.server_tick = 7;
    hit.source_net_id = kProjectile;
    hit.target_net_id = kSwordsman;
    hit.damage = 12;
    hit.hit_position = glm::vec3{1.0f, 2.0f, 3.0f};
    engine.applied_damage_.push_back(hit);
    KernelDamageRecord record{};
    record.struct_size = sizeof(record);
    require(engine.server_query_damage_records(&record, 1u) == 1u);
    require(record.attacker_net_id == kShooter);
    require(record.victim_net_id == kSwordsman);
    require(record.damage == 12u && record.tick == 7u);
    require(record.hit_position.y == 2.0f);
    // An unsized buffer is refused, and a null one only counts.
    KernelDamageRecord unsized{};
    require(engine.server_query_damage_records(&unsized, 1u) == 0u);
    require(engine.server_query_damage_records(nullptr, 0u) == 1u);

    std::printf("damage_record_shooter_test passed\n");
    return 0;
}
