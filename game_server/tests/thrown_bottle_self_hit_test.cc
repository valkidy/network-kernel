// A thrown bottle must not go off on the player who threw it.
//
// The shipped catalog, driven through the kernel API. A thrown prop starts at
// its thrower + (0, 1, 0), inside the thrower's own hitbox. Measured
// 2026-09-25, before the fix: a level or rising throw left the hitbox cleanly,
// but any downward throw -- 10 degrees was enough -- collided with the thrower
// on its first tick and detonated there. The frag bottle deals 1000 to every
// side, so that was the thrower dead. The in-flight collision check now skips
// ThrownPropMotion::thrower_net_id.
//
// The downward case is not about throwing at your own feet: there is no
// terrain here, so the only thing the bottle could hit is the thrower's body.
// A frag that lands at your feet still kills you; that is the blast, which has
// no shooter to skip, not the bottle.
//
// The level throw's bystander is the control. It shows the bottle does
// detonate and the blast does deal damage, so a thrower left at full health
// means "not hit" rather than "nothing happened".

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "game_server/src/game_server.h"
#include "game_server/src/gameplay_config.h"
#include "kernel/public/kernel_api.h"

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

constexpr float kTickSeconds = 1.0f / 30.0f;
constexpr std::uint32_t kPeer = 7;

std::uint32_t item_template_id_of(
    const network_example::game_server::GameServerGameplayConfig& config,
    const std::string& name) {
    for (const network_example::game_server::ItemTemplateConfig& candidate :
         config.item_templates) {
        if (candidate.name == name) {
            return candidate.definition.item_template_id;
        }
    }
    return 0;
}

std::uint32_t spawn_player(
    KernelHandle* kernel,
    std::uint32_t template_id,
    const KernelVec3& position) {
    KernelServerEntityCreateInfo create{};
    create.struct_size = sizeof(create);
    create.entity_type = network_example::game_server::kEntityTypeActor;
    create.actor_type = network_example::game_server::kActorTypePlayer;
    create.entity_template_id = template_id;
    create.actor_template_id = template_id;
    create.owner_peer = kPeer;
    create.position = position;
    create.rotation = KernelQuat{0.0f, 0.0f, 0.0f, 1.0f};
    std::uint32_t net_id = 0;
    require(Kernel_ServerCreateEntity(kernel, &create, &net_id));
    require(net_id != 0);
    require(Kernel_ServerSetEntityActorTemplate(kernel, net_id, template_id));
    return net_id;
}

std::uint16_t hp_of(KernelHandle* kernel, std::uint32_t net_id) {
    KernelServerEntityState state{};
    state.struct_size = sizeof(state);
    if (!Kernel_ServerGetEntityState(kernel, net_id, &state)) {
        return 0;
    }
    return state.hp;
}

struct ThrowResult {
    std::uint16_t thrower_hp_before = 0;
    std::uint16_t thrower_hp_after = 0;
    std::uint16_t bystander_hp_before = 0;
    std::uint16_t bystander_hp_after = 0;
};

// Throws one `item_name` from the origin along `direction` and runs `ticks`.
// A bystander stands `bystander_x` metres down +x, or none if zero.
ThrowResult throw_once(
    const network_example::game_server::GameServerGameplayConfig& config,
    const std::string& item_name,
    const KernelVec3& direction,
    float bystander_x,
    int ticks,
    std::uint16_t port) {
    KernelConfig kernel_config{};
    kernel_config.mode = KernelMode_DedicatedServer;
    kernel_config.tick.server_tick_rate = 30;
    kernel_config.tick.snapshot_rate = 15;
    kernel_config.max_events = 256;
    kernel_config.max_render_states = 64;
    KernelHandle* kernel = Kernel_Create(&kernel_config);
    require(kernel != nullptr);
    require(Kernel_StartDedicatedServer(kernel, port));
    require(network_example::game_server::load_kernel_gameplay_catalog(
        kernel, config));

    const std::uint32_t player_template = config.player.actor_template_id;
    const std::uint32_t thrower =
        spawn_player(kernel, player_template, KernelVec3{0.0f, 0.0f, 0.0f});
    std::uint32_t bystander = 0;
    if (bystander_x != 0.0f) {
        bystander = spawn_player(
            kernel, player_template, KernelVec3{bystander_x, 0.0f, 0.0f});
    }

    const std::uint32_t item_template = item_template_id_of(config, item_name);
    require(item_template != 0);
    KernelInventoryContainerId container = 0;
    require(Kernel_ServerCreateInventoryContainer(kernel, thrower, 8, &container));
    KernelItemInstanceId item = 0;
    require(Kernel_ServerCreateInventoryItem(
        kernel, item_template, 1, container, &item));

    // One tick so every collider is in the physics world before the throw.
    Kernel_Update(kernel, kTickSeconds);

    ThrowResult result;
    result.thrower_hp_before = hp_of(kernel, thrower);
    result.bystander_hp_before = bystander == 0 ? 0 : hp_of(kernel, bystander);

    KernelGameplayRequest throw_request{};
    throw_request.struct_size = sizeof(throw_request);
    throw_request.requester_peer = kPeer;
    throw_request.request_id = 1;
    throw_request.instigator_net_id = thrower;
    throw_request.domain_action = KernelDomainAction_Throw;
    throw_request.selected_item_instance_id = item;
    throw_request.requested_quantity = 1;
    throw_request.throw_direction = direction;
    require(Kernel_ServerSubmitGameplayRequest(kernel, &throw_request));

    KernelGameplayRequestOutcome outcomes[4]{};
    for (KernelGameplayRequestOutcome& outcome : outcomes) {
        outcome.struct_size = sizeof(outcome);
    }
    const std::uint32_t outcome_count =
        Kernel_PollGameplayRequestOutcomes(kernel, outcomes, 4);
    require(outcome_count == 1u);
    require(outcomes[0].status == KernelGameplayRequestStatus_Committed);
    require(outcomes[0].prop_entity_id != 0u);

    for (int tick = 0; tick < ticks; ++tick) {
        Kernel_Update(kernel, kTickSeconds);
    }
    result.thrower_hp_after = hp_of(kernel, thrower);
    result.bystander_hp_after = bystander == 0 ? 0 : hp_of(kernel, bystander);

    std::fprintf(
        stderr,
        "%s: thrower %u -> %u, bystander %u -> %u\n",
        item_name.c_str(),
        result.thrower_hp_before,
        result.thrower_hp_after,
        result.bystander_hp_before,
        result.bystander_hp_after);

    Kernel_Destroy(kernel);
    return result;
}

}  // namespace

int main() {
    const network_example::game_server::GameServerGameplayConfig config =
        network_example::game_server::default_game_server_gameplay_config();

    // Level, at a player 4.5 m away -- outside the 3.5 m frag radius from
    // the thrower, so only the bystander should be hurt.
    const ThrowResult frag = throw_once(
        config,
        "fungible_frag_grenade_bottle",
        KernelVec3{1.0f, 0.0f, 0.0f},
        4.5f,
        15,
        7881);
    require(frag.thrower_hp_before > 0u);
    require(frag.bystander_hp_before > 0u);
    // The control: the bottle detonated and the blast dealt damage.
    require(frag.bystander_hp_after < frag.bystander_hp_before);
    // The claim: the thrower was not the one it went off on.
    require(frag.thrower_hp_after == frag.thrower_hp_before);

    // A rising throw leaves the hitbox cleanly too.
    const float up = 30.0f * 3.14159265f / 180.0f;
    const ThrowResult rising = throw_once(
        config,
        "fungible_frag_grenade_bottle",
        KernelVec3{std::cos(up), std::sin(up), 0.0f},
        0.0f,
        15,
        7882);
    require(rising.thrower_hp_after == rising.thrower_hp_before);

    // Ten degrees down, at an enemy a few metres off. Nothing else is in the
    // world, so any damage here is the bottle going off on the thrower.
    const float down = 10.0f * 3.14159265f / 180.0f;
    const ThrowResult falling = throw_once(
        config,
        "fungible_frag_grenade_bottle",
        KernelVec3{std::cos(down), -std::sin(down), 0.0f},
        0.0f,
        15,
        7883);
    require(falling.thrower_hp_after == falling.thrower_hp_before);

    std::puts("thrown_bottle_self_hit_test passed");
    return 0;
}
