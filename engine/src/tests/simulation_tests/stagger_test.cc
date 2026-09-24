// Hit stagger: a meter every landed hit fills, and crossing its threshold
// interrupts whatever the actor was doing and refuses new actions for a while.
// Also the knockback half of the same gate: an ImpulseLockout refuses new
// actions too, but leaves the one already in flight alone.
//
// Every interrupt case here pairs with a control that runs the identical
// action without the hit and watches it commit, because "no commit happened"
// is also what a broken template or a missing input looks like.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "kernel/public/kernel_types.h"
#include "simulation/public/simulation.h"

namespace {

using namespace network_example;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "stagger_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

constexpr std::uint32_t kSwingTemplateId = 3001u;
constexpr std::uint32_t kWindupTicks = 5u;
constexpr std::uint32_t kRecoveryTicks = 8u;
constexpr std::uint32_t kStaggerTicks = 10u;
constexpr std::uint32_t kImmunityTicks = 20u;
constexpr float kThreshold = 100.0f;
constexpr PeerId kPlayerPeer = 1u;

// A single-commit swing with a windup, and deliberately *without*
// CancelBeforeFirstCommit: through the ordinary cancel path this template
// would still swing once before stopping, which is exactly what a stagger
// must not allow.
RuntimeActionTemplate swing_template() {
    return RuntimeActionTemplate{
        kSwingTemplateId,
        KernelActionTriggerMode_Press,
        static_cast<std::uint8_t>(KernelActionTemplateFlag_CancelOnDeath),
        0u,
        kWindupTicks,
        0u,
        1u,
        kRecoveryTicks,
        0u,
    };
}

void arm(World& world, NetId net_id) {
    const entt::entity entity = *world.find_entity(net_id);
    world.registry().get_or_emplace<Health>(entity) = Health{500, 500};
    WeaponTuning& tuning = world.registry().get_or_emplace<WeaponTuning>(entity);
    tuning.configured = {true, false, false, false, false, false, false};
    WeaponMechanicsDefinition weapon;
    weapon.id = kWeaponSlot0;
    weapon.mode = WeaponFireMode::kHitscan;
    weapon.magazine_size = 100;
    weapon.damage = 10;
    weapon.max_range = 10.0f;
    weapon.pellet_count = 1;
    weapon.fire_action_template_id = kSwingTemplateId;
    weapon.reload_action_template_id = 0u;
    tuning.definitions[kWeaponSlot0] = weapon;
    WeaponState& state = world.registry().get_or_emplace<WeaponState>(entity);
    state.weapon_slot_count = 1;
    state.weapon_ids[0] = kWeaponSlot0;
    state.ammo[0] = weapon.magazine_size;
    state.reserve_magazines[0] = 1;
}

void give_profile(World& world, NetId net_id) {
    StaggerProfile profile;
    profile.threshold = kThreshold;
    profile.stagger_per_damage = 1.0f;
    profile.decay_per_tick = 5.0f;
    profile.decay_delay_ticks = 10u;
    profile.stagger_ticks = kStaggerTicks;
    profile.immunity_ticks = kImmunityTicks;
    world.registry().emplace_or_replace<StaggerProfile>(
        *world.find_entity(net_id), profile);
}

struct Fixture {
    Fixture() {
        world.set_action_templates({swing_template()});
        player = world.spawn_player(kPlayerPeer, glm::vec3{0.0f});
        arm(world, player);
        give_profile(world, player);
    }

    KernelPlayerInput idle_input() const {
        KernelPlayerInput input{};
        input.input_seq = next_seq;
        input.aim_dir = KernelVec3{1.0f, 0.0f, 0.0f};
        input.selected_weapon = kWeaponSlot0;
        return input;
    }

    KernelPlayerInput press(std::uint32_t action_instance_id) const {
        KernelPlayerInput input = idle_input();
        input.action_intent = KernelActionIntent{
            action_instance_id, KernelActionBinding_PrimaryFire, 0u, 0u};
        return input;
    }

    // One tick of the action pass, then the end-of-tick damage the kernel
    // applies after it -- the same order simulate_tick runs them in.
    void step(
        const std::vector<QueuedInput>& inputs,
        const std::vector<ConfirmedDamage>& hits = {}) {
        const std::vector<ActionCommit> tick_commits =
            simulate_actions(world, inputs, tick, &outcomes);
        commits += static_cast<int>(tick_commits.size());
        apply_damage_applications(world, hits, tick, &events);
        ++tick;
        ++next_seq;
    }

    void step_player(
        const KernelPlayerInput& input,
        const std::vector<ConfirmedDamage>& hits = {}) {
        step({QueuedInput{kPlayerPeer, input}}, hits);
    }

    ConfirmedDamage hit(
        NetId target,
        std::uint16_t damage,
        float stagger = kStaggerDerivedFromDamage) const {
        ConfirmedDamage confirmed;
        confirmed.server_tick = tick;
        confirmed.target_net_id = target;
        confirmed.source_peer = 9u;
        confirmed.damage = damage;
        confirmed.stagger = stagger;
        return confirmed;
    }

    const ActionRuntimeState& action(NetId net_id) const {
        return world.registry().get<ActionRuntimeState>(*world.find_entity(net_id));
    }

    int outcomes_with(ActionOutcomeType type, KernelLocalActionResultReason reason) const {
        int count = 0;
        for (const ActionOutcome& outcome : outcomes) {
            if (outcome.type == type && outcome.reason == reason) {
                ++count;
            }
        }
        return count;
    }

    int staggered_events() const {
        int count = 0;
        for (const KernelEvent& event : events) {
            if (event.type == KernelEventType_Staggered) {
                require(event.code == kStaggerTicks);
                ++count;
            }
        }
        return count;
    }

    std::uint32_t visual_flags(NetId net_id) const {
        const ReplicationState* replication =
            world.registry().try_get<ReplicationState>(*world.find_entity(net_id));
        return replication == nullptr ? 0u : replication->visual_flags;
    }

    World world;
    NetId player = 0;
    std::uint32_t tick = 100u;
    std::uint32_t next_seq = 1u;
    int commits = 0;
    std::vector<ActionOutcome> outcomes;
    std::vector<KernelEvent> events;
};

// ---------------------------------------------------------------------------
// The meter
// ---------------------------------------------------------------------------

// The control for every interrupt below: the swing, a sub-threshold hit during
// its windup, and the swing still lands on schedule.
void a_hit_below_threshold_does_not_interrupt() {
    Fixture f;
    f.step_player(f.press(1u));
    f.step_player(f.idle_input(), {f.hit(f.player, 60u)});
    for (std::uint32_t i = 0; i < kWindupTicks + 2u; ++i) {
        f.step_player(f.idle_input());
    }
    require(f.commits == 1);
    require(f.staggered_events() == 0);
    require(!is_staggered(f.world, *f.world.find_entity(f.player), f.tick));
}

void crossing_the_threshold_interrupts_the_windup_without_swinging() {
    Fixture f;
    f.step_player(f.press(1u));
    require(f.action(f.player).phase == KernelActionPhase_Windup);
    // Two hits in one tick crossing together -- the meter sums, it does not
    // judge hits one at a time.
    f.step_player(f.idle_input(), {f.hit(f.player, 60u), f.hit(f.player, 60u)});
    require(f.staggered_events() == 1);
    require((f.visual_flags(f.player) & kVisualFlagStaggered) != 0u);

    for (std::uint32_t i = 0; i < kWindupTicks + 2u; ++i) {
        f.step_player(f.idle_input());
    }
    require(f.commits == 0);
    require(
        f.outcomes_with(
            ActionOutcomeType::Corrected, KernelLocalActionResultReason_Staggered) == 1);
    // Recovery skipped: the interrupted action is gone, not recovering.
    require(f.action(f.player).phase == KernelActionPhase_None);
}

void a_new_action_is_refused_while_staggered_and_admitted_after() {
    Fixture f;
    f.step_player(f.idle_input(), {f.hit(f.player, 150u)});
    const std::uint32_t staggered_from = f.tick;

    f.step_player(f.press(1u));
    require(
        f.outcomes_with(
            ActionOutcomeType::Rejected, KernelLocalActionResultReason_Staggered) == 1);
    require(f.action(f.player).phase == KernelActionPhase_None);

    // Exactly kStaggerTicks ticks are held, counted from the first tick after
    // the hit landed.
    while (f.tick < staggered_from + kStaggerTicks) {
        f.step_player(f.idle_input());
    }
    require((f.visual_flags(f.player) & kVisualFlagStaggered) != 0u);
    f.step_player(f.press(2u));
    require(
        f.outcomes_with(ActionOutcomeType::Admitted, KernelLocalActionResultReason_None) == 1);
    require((f.visual_flags(f.player) & kVisualFlagStaggered) == 0u);
}

void immunity_prevents_chained_staggers() {
    Fixture f;
    // One damage, authored stagger: the target has to survive thirty-odd hits.
    f.step_player(f.idle_input(), {f.hit(f.player, 1u, 150.0f)});
    require(f.staggered_events() == 1);
    // Hammer the target through the stagger and the immunity after it.
    for (std::uint32_t i = 0; i < kStaggerTicks + kImmunityTicks; ++i) {
        f.step_player(f.idle_input(), {f.hit(f.player, 1u, 150.0f)});
    }
    require(f.staggered_events() == 1);
    // Immunity over: the very next heavy hit staggers again.
    f.step_player(f.idle_input(), {f.hit(f.player, 1u, 150.0f)});
    require(f.staggered_events() == 2);
}

void the_meter_decays_between_hits() {
    // Two 60s back to back cross 100; the same two with a long gap do not.
    Fixture quick;
    quick.step_player(quick.idle_input(), {quick.hit(quick.player, 60u)});
    quick.step_player(quick.idle_input(), {quick.hit(quick.player, 60u)});
    require(quick.staggered_events() == 1);

    Fixture slow;
    slow.step_player(slow.idle_input(), {slow.hit(slow.player, 60u)});
    // decay_delay 10 + 20 more ticks at 5/tick drains all 60.
    for (int i = 0; i < 30; ++i) {
        slow.step_player(slow.idle_input());
    }
    slow.step_player(slow.idle_input(), {slow.hit(slow.player, 60u)});
    require(slow.staggered_events() == 0);
}

void an_authored_stagger_overrides_the_damage_derived_one() {
    Fixture zero;
    zero.step_player(zero.idle_input(), {zero.hit(zero.player, 400u, 0.0f)});
    require(zero.staggered_events() == 0);

    Fixture heavy;
    heavy.step_player(heavy.idle_input(), {heavy.hit(heavy.player, 1u, 150.0f)});
    require(heavy.staggered_events() == 1);
}

void an_actor_without_a_profile_is_never_staggered() {
    Fixture f;
    f.world.registry().remove<StaggerProfile>(*f.world.find_entity(f.player));
    f.step_player(f.idle_input(), {f.hit(f.player, 400u)});
    require(f.staggered_events() == 0);
    // The hit itself still landed.
    require(
        f.world.registry().get<Health>(*f.world.find_entity(f.player)).hp == 100u);
}

// The action pass only advances actors that sent input this tick. An AI agent
// that is not attacking may well send none, and its interrupt must not wait
// for it to.
void an_agent_is_interrupted_on_a_tick_it_sent_no_input() {
    Fixture f;
    const NetId agent = f.world.spawn_enemy(glm::vec3{3.0f, 0.0f, 0.0f});
    arm(f.world, agent);
    give_profile(f.world, agent);

    QueuedInput agent_press{kPlayerPeer + 1u, f.press(1u)};
    agent_press.controlled_net_id = agent;
    f.step({agent_press});
    require(f.action(agent).phase == KernelActionPhase_Windup);

    // From here only the player talks, so the agent is never advanced. Were
    // the interrupt tied to advance_action, the agent would sit in Windup
    // until its next input and swing the moment it sent one.
    f.step_player(f.idle_input(), {f.hit(agent, 150u)});
    for (std::uint32_t i = 0; i < kWindupTicks + 2u; ++i) {
        f.step_player(f.idle_input());
    }
    require(f.action(agent).phase == KernelActionPhase_None);
    require(
        f.outcomes_with(
            ActionOutcomeType::Corrected, KernelLocalActionResultReason_Staggered) == 1);
}

// ---------------------------------------------------------------------------
// Knockback shares the admission gate but not the interrupt
// ---------------------------------------------------------------------------

void a_knockback_lets_the_swing_in_flight_finish() {
    Fixture f;
    f.step_player(f.press(1u));
    f.world.registry().emplace<ImpulseLockout>(
        *f.world.find_entity(f.player), ImpulseLockout{f.tick + 20u, f.tick});
    for (std::uint32_t i = 0; i < kWindupTicks + 2u; ++i) {
        f.step_player(f.idle_input());
    }
    require(f.commits == 1);
}

void a_knockback_refuses_new_actions_until_it_ends() {
    Fixture f;
    const entt::entity entity = *f.world.find_entity(f.player);
    f.world.registry().emplace<ImpulseLockout>(
        entity, ImpulseLockout{f.tick + 3u, f.tick});
    f.step_player(f.press(1u));
    require(
        f.outcomes_with(
            ActionOutcomeType::Rejected, KernelLocalActionResultReason_KnockedBack) == 1);

    // Landing reaps the component (player_movement does it); gone means free.
    f.world.registry().remove<ImpulseLockout>(entity);
    f.step_player(f.press(2u));
    require(
        f.outcomes_with(ActionOutcomeType::Admitted, KernelLocalActionResultReason_None) == 1);
}

void a_stagger_outranks_a_knockback_in_the_rejection_reason() {
    Fixture f;
    f.world.registry().emplace<ImpulseLockout>(
        *f.world.find_entity(f.player), ImpulseLockout{f.tick + 20u, f.tick});
    f.step_player(f.idle_input(), {f.hit(f.player, 150u)});
    f.step_player(f.press(1u));
    require(
        f.outcomes_with(
            ActionOutcomeType::Rejected, KernelLocalActionResultReason_Staggered) == 1);
    require(
        f.outcomes_with(
            ActionOutcomeType::Rejected, KernelLocalActionResultReason_KnockedBack) == 0);
}

}  // namespace

int main() {
    a_hit_below_threshold_does_not_interrupt();
    crossing_the_threshold_interrupts_the_windup_without_swinging();
    a_new_action_is_refused_while_staggered_and_admitted_after();
    immunity_prevents_chained_staggers();
    the_meter_decays_between_hits();
    an_authored_stagger_overrides_the_damage_derived_one();
    an_actor_without_a_profile_is_never_staggered();
    an_agent_is_interrupted_on_a_tick_it_sent_no_input();
    a_knockback_lets_the_swing_in_flight_finish();
    a_knockback_refuses_new_actions_until_it_ends();
    a_stagger_outranks_a_knockback_in_the_rejection_reason();
    std::puts("stagger_test: ok");
    return 0;
}
