// What a client knows about held weapons (design D27, snapshot schema 28).
//
// A pure client never has the local player in its world, so it has no loadout
// of its own to turn the snapshot's active slot into a weapon. The player
// record now names the weapon, which is what Kernel_GetLocalWeaponState
// reports -- and what is right straight after a pickup or swap, which no
// template-built loadout could know. Every player's render state carries the
// same byte, for drawing the weapon in their hands; the authority's render
// states take it from the world.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/src/render_state_builder.h"

#define private public
#include "kernel/src/kernel.h"
#undef private

namespace {

namespace ne = network_example;

void require_impl(bool condition, const char* expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, expression);
    std::abort();
}
#define require(condition) require_impl((condition), #condition, __LINE__)

ne::EntitySnapshot own_record(ne::NetId player) {
    ne::EntitySnapshot own;
    own.net_id = player;
    own.type = ne::EntityType::kActor;
    own.actor_type = ne::ActorType::kPlayer;
    own.has_owner_weapon_state = true;
    own.active_weapon_slot = 1;
    own.active_weapon_ammo = 2;
    own.has_held_weapon = true;
    own.held_weapon_id = 13;
    return own;
}

void pure_client_names_the_weapon_from_the_snapshot() {
    KernelConfig config{};
    config.mode = KernelMode_Client;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    ne::KernelEngine client(config);
    client.reset_runtime_state(KernelMode_Client);
    client.local_player_net_id_ = 42;  // never spawned into this world
    KernelLocalWeaponState state{};
    state.struct_size = sizeof(state);

    ne::WorldSnapshot snapshot;
    snapshot.header.server_tick = 50;
    snapshot.entities.push_back(own_record(42));
    client.apply_authoritative_local_weapon(snapshot);
    require(client.local_weapon_state(&state));
    require((state.flags & KERNEL_LOCAL_WEAPON_STATE_FLAG_WEAPON_ID_VALID) != 0u);
    require(state.weapon_id == 13u);
    require(state.authoritative_ammo == 2u);

    // A swap: same slot index, another weapon. The id follows the record.
    snapshot.header.server_tick = 52;
    snapshot.entities[0].held_weapon_id = 5;
    client.apply_authoritative_local_weapon(snapshot);
    require(client.local_weapon_state(&state));
    require(state.weapon_id == 5u);

    // A record that does not say leaves the id unknown, as before schema 28.
    snapshot.header.server_tick = 54;
    snapshot.entities[0].has_held_weapon = false;
    client.apply_authoritative_local_weapon(snapshot);
    require(client.local_weapon_state(&state));
    require((state.flags & KERNEL_LOCAL_WEAPON_STATE_FLAG_WEAPON_ID_VALID) == 0u);

    // Unarmed: no weapon block, nothing to report.
    snapshot.header.server_tick = 56;
    snapshot.entities[0].has_owner_weapon_state = false;
    snapshot.entities[0].has_held_weapon = true;
    snapshot.entities[0].held_weapon_id = KERNEL_HELD_WEAPON_NONE;
    client.apply_authoritative_local_weapon(snapshot);
    require(!client.local_weapon_state(&state));
}

void render_states_carry_it() {
    ne::EntitySnapshot teammate = own_record(7);
    teammate.has_owner_weapon_state = false;
    RenderEntityState drawn = ne::render_state_from_snapshot_entity(teammate, 7u);
    require(drawn.has_held_weapon == 1u);
    require(drawn.held_weapon_id == 13u);
    teammate.has_held_weapon = false;
    drawn = ne::render_state_from_snapshot_entity(teammate, 7u);
    require(drawn.has_held_weapon == 0u);

    // The authority's own render states read the world.
    KernelConfig config{};
    config.mode = KernelMode_DedicatedServer;
    config.tick.server_tick_rate = 30;
    config.tick.snapshot_rate = 15;
    ne::KernelEngine server(config);
    server.reset_runtime_state(KernelMode_DedicatedServer);
    const ne::NetId player = server.world_.spawn_player(3, glm::vec3{0.0f});
    const entt::entity entity = *server.world_.find_entity(player);
    ne::WeaponState& weapon = server.world_.registry().get<ne::WeaponState>(entity);
    weapon.weapon_slot_count = 2;
    weapon.weapon_ids[0] = 0;
    weapon.weapon_ids[1] = 14;
    weapon.active_weapon_slot = 1;
    drawn = ne::render_state_from_world_entity(server.world_, entity, 1u);
    require(drawn.has_held_weapon == 1u);
    require(drawn.held_weapon_id == 14u);
    weapon.weapon_slot_count = 0;
    weapon.active_weapon_slot = 0;
    drawn = ne::render_state_from_world_entity(server.world_, entity, 1u);
    require(drawn.has_held_weapon == 1u);
    require(drawn.held_weapon_id == KERNEL_HELD_WEAPON_NONE);
}

}  // namespace

int main() {
    pure_client_names_the_weapon_from_the_snapshot();
    render_states_carry_it();
    std::puts("held_weapon_client_test passed");
    return 0;
}
