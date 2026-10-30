#include "game_server/src/agent_chaser_controller.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include "ai_intent.h"
#include "game_server/src/actor_intent_executor.h"
#include "game_server/src/agent_behavior.h"
#include "game_server/src/agent_steering.h"
#include "game_server/src/ai_perception_adapter.h"
#include "game_server/src/patrol_navigation.h"
#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {
namespace {

bool in_reach(const AgentWeaponSkill& weapon, float distance) {
    return distance >= weapon.range_min_meters &&
        (weapon.range_max_meters <= 0.0f || distance <= weapon.range_max_meters);
}

bool has_ammo(const KernelServerEntityState& self, std::uint16_t weapon_id) {
    for (std::uint8_t slot = 0; slot < self.weapon_slot_count; ++slot) {
        if (self.weapon_ids[slot] == weapon_id) {
            return self.ammo[slot] > 0u || self.reserve_magazines[slot] > 0u;
        }
    }
    return false;
}

// Mid-swing or reloading: the kernel would not take the weapon out of its hand
// (hold_selected_weapon), and a change of weapon cancels a hold.
bool busy(const KernelServerEntityState& self) {
    return self.is_reloading != 0u ||
        self.action.phase == KernelActionPhase_Windup ||
        self.action.phase == KernelActionPhase_Active;
}

std::uint32_t next_random(AgentRuntimeState* agent) {
    std::uint32_t x = agent->skill_random != 0u
        ? agent->skill_random
        : agent->net_id * 2654435761u + 0x9e3779b9u;
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    agent->skill_random = x != 0u ? x : 1u;
    return agent->skill_random;
}

// D14: keeps the weapon in hand while it can still be used and the agent has
// not just fired it; otherwise, once the agent is free to switch, draws one by
// weight from the skills that are in reach, off cooldown and loaded. With none
// usable it keeps what it has, and the pursuit closes instead.
void choose_weapon_skill(
    const std::vector<AgentWeaponSkill>& skills,
    const KernelServerEntityState& self,
    float distance,
    AgentRuntimeState* agent) {
    const auto usable = [&](std::size_t index) {
        return in_reach(skills[index], distance) &&
            agent->skill_cooldown_ticks[index] == 0u &&
            has_ammo(self, skills[index].executor.weapon_id);
    };
    if (agent->weapon_skill >= skills.size()) {
        agent->weapon_skill = 0;
    }
    if ((!agent->choose_weapon && usable(agent->weapon_skill)) || busy(self)) {
        return;
    }
    std::uint32_t total = 0;
    for (std::size_t index = 0; index < skills.size(); ++index) {
        if (usable(index)) total += skills[index].weight;
    }
    if (total == 0u) {
        return;
    }
    std::uint32_t pick = next_random(agent) % total;
    for (std::size_t index = 0; index < skills.size(); ++index) {
        if (!usable(index)) continue;
        if (pick < skills[index].weight) {
            agent->weapon_skill = static_cast<std::uint8_t>(index);
            agent->choose_weapon = false;
            return;
        }
        pick -= skills[index].weight;
    }
}

// How close Search comes to where the target was last seen before it stops
// and looks around. The same allowance a route corner gets.
constexpr float kSearchArrivalMeters = 1.0f;

bool is_zero_move(const KernelVec2& move) {
    return move.x == 0.0f && move.y == 0.0f;
}

// Distance from where the agent broke off, which is what the leash bounds. A
// leash of zero means no leash.
bool leash_exceeded(float leash_meters, const AgentRuntimeState& agent) {
    return leash_meters > 0.0f &&
        agent_steering::horizontal_distance(
            agent.position, agent.patrol.leash_anchor) > leash_meters;
}

// Mirrors leap_airtime_ticks in simulation/public/simulation.h, which
// game_server does not link: the airtime of a leap that peaks apex_meters over
// the higher of its two ends, stretched to keep under the horizontal speed
// cap. The kernel takes the result as given, so the two only have to agree to
// keep tuning meaningful.
std::uint32_t leap_airtime_ticks(
    const AgentLeapTuning& leap,
    float horizontal_distance,
    float height_difference,
    float gravity_y,
    float fixed_delta_seconds) {
    float seconds = 0.0f;
    if (leap.apex_meters > 0.0f && gravity_y < 0.0f) {
        const float rise =
            leap.apex_meters + std::max(0.0f, height_difference);
        const float fall =
            leap.apex_meters + std::max(0.0f, -height_difference);
        seconds = std::sqrt(2.0f * rise / -gravity_y) +
            std::sqrt(2.0f * fall / -gravity_y);
    }
    if (leap.max_horizontal_speed_meters_per_second > 0.0f) {
        seconds = std::max(
            seconds,
            horizontal_distance / leap.max_horizontal_speed_meters_per_second);
    }
    const float ticks = std::ceil(seconds / fixed_delta_seconds);
    // The kernel refuses a one-step leap.
    return ticks < 2.0f ? 2u : static_cast<std::uint32_t>(ticks);
}

// Starts a leap at the target if it is in the band and the agent is free to.
// The landing spot is fixed here, at lift-off: a target that moves after the
// agent commits is where the leap misses. Returns whether one was enqueued;
// the kernel may still refuse it, which the next tick's flags show.
//
// target_feet is the target's own position, not perception's target_position:
// that is an aim point at the body's centre, and read as the ground it put a
// level leap 0.9 m uphill and stretched its airtime to match.
bool try_start_leap(
    KernelHandle* kernel,
    const AgentChaserConfig& config,
    AgentRuntimeState* agent,
    const KernelServerEntityState& self,
    const KernelVec3& target_feet) {
    const AgentLeapTuning& leap = config.chase.leap;
    if (leap.max_distance_meters <= 0.0f || agent->leap_ticks_to_landing > 0u ||
        agent->leap_cooldown_ticks > 0u ||
        (self.visual_flags & KERNEL_VISUAL_FLAG_GROUNDED) == 0u ||
        (self.visual_flags & KERNEL_VISUAL_FLAG_KNOCKED_BACK) != 0u) {
        return false;
    }
    const float dx = target_feet.x - agent->position.x;
    const float dz = target_feet.z - agent->position.z;
    const float distance = std::hypot(dx, dz);
    if (distance < leap.min_distance_meters ||
        distance > leap.max_distance_meters || distance <= 0.0001f) {
        return false;
    }
    const float travel = distance - leap.landing_distance_meters;
    const KernelVec3 destination{
        agent->position.x + dx * (travel / distance),
        target_feet.y,
        agent->position.z + dz * (travel / distance)};
    // The target's feet stand in for the ground under the landing spot,
    // which only the kernel probes.
    const std::uint32_t airtime = leap_airtime_ticks(
        leap,
        travel,
        target_feet.y - agent->position.y,
        config.gravity_y,
        Kernel_GetFixedDeltaSeconds(kernel));
    if (!Kernel_ServerEnqueueEntityLeap(
            kernel, KernelCommandSource_AI, agent->net_id, &destination, airtime)) {
        return false;
    }
    agent->leap_ticks_to_landing = airtime;
    agent->leap_cooldown_ticks = airtime + leap.cooldown_ticks;
    return true;
}

// Starts winding up a charge if the target is in the band and the agent is
// free to: on the ground, not knocked back or staggered, not leaping, and off
// cooldown. The run itself is released by release_charge once the wind-up is
// spent.
bool try_start_charge(
    const AgentChaserConfig& config,
    AgentRuntimeState* agent,
    const KernelServerEntityState& self,
    const KernelVec3& target_feet) {
    const AgentChargeTuning& charge = config.chase.charge;
    if (charge.max_distance_meters <= 0.0f || agent->charge_cooldown_ticks > 0u ||
        agent->charge_windup_ticks > 0u || agent->charge_ticks_left > 0u ||
        agent->leap_ticks_to_landing > 0u ||
        (self.visual_flags & KERNEL_VISUAL_FLAG_GROUNDED) == 0u ||
        (self.visual_flags & (KERNEL_VISUAL_FLAG_KNOCKED_BACK |
                              KERNEL_VISUAL_FLAG_STAGGERED)) != 0u) {
        return false;
    }
    const float distance = std::hypot(
        target_feet.x - agent->position.x, target_feet.z - agent->position.z);
    if (distance < charge.min_distance_meters ||
        distance > charge.max_distance_meters) {
        return false;
    }
    // Plus one: release_charge spends a tick of it before it looks.
    agent->charge_windup_ticks = charge.windup_ticks + 1u;
    return true;
}

// Releases the run at where the target stands now: the line is fixed here, so
// a target that moves after this is where the charge misses. Long enough to
// pass overshoot_meters beyond that spot. Whatever the kernel says, the agent
// goes on cooldown, so a refused charge is not retried every tick.
void release_charge(
    KernelHandle* kernel,
    const AgentChaserConfig& config,
    AgentRuntimeState* agent,
    const KernelVec3& target_feet) {
    const AgentChargeTuning& charge = config.chase.charge;
    agent->charge_cooldown_ticks = charge.cooldown_ticks;
    const float dx = target_feet.x - agent->position.x;
    const float dz = target_feet.z - agent->position.z;
    const float distance = std::hypot(dx, dz);
    if (distance <= 0.0001f) {
        return;
    }
    const float seconds =
        (distance + charge.overshoot_meters) / charge.speed_meters_per_second;
    const float ticks = std::ceil(seconds / Kernel_GetFixedDeltaSeconds(kernel));
    const std::uint32_t duration = static_cast<std::uint32_t>(std::clamp(
        ticks, 1.0f, static_cast<float>(KERNEL_MAX_IMPULSE_LOCKOUT_TICKS)));
    const KernelVec3 direction{dx / distance, 0.0f, dz / distance};
    if (!Kernel_ServerEnqueueEntityCharge(
            kernel,
            KernelCommandSource_AI,
            agent->net_id,
            &direction,
            charge.speed_meters_per_second,
            duration,
            charge.weapon_id)) {
        return;
    }
    agent->charge_ticks_left = duration;
    agent->charge_direction = direction;
    agent->charge_cooldown_ticks = duration + charge.cooldown_ticks;
}

// Where one agent of a spaced template stands this tick (chase.spacing_meters):
// its spot on the ring round a target it shares, and the push away from the
// others of its template that are too close.
struct ChaseSpacing {
    bool has_slot = false;
    KernelVec3 slot{0.0f, 0.0f, 0.0f};
    KernelVec2 push{0.0f, 0.0f};
};

// A push weaker than this is left out of the move: two agents parked in their
// slots sit a little inside the spacing whenever they stopped short of them,
// and acting on that would walk them on and off the spot every tick.
constexpr float kSpacingPushDeadzone = 0.15f;

// How far off the line to its target a spaced agent may walk. A legged body
// turns to where it walks and its eyes turn with it, so a giant sent round
// the target to its slot looked away (its cone is 140 degrees), lost sight,
// fell back to Search and walked straight in to where it last saw it. Inside
// the giant's half-cone of 70, with room for the body's turn lagging behind.
constexpr float kSpacingMaxOffTargetDegrees = 65.0f;

// The ring's spots and every agent's push, worked out once for the batch
// before any agent moves, from where they all stood at the start of the tick.
// Who is pursuing what is last tick's answer: this tick's is only known once
// each agent has run.
std::vector<ChaseSpacing> plan_chase_spacing(
    const AgentChaseTuning& chase,
    const PerceptionFrame& frame,
    const AgentBatch& agents) {
    std::vector<ChaseSpacing> spacing(agents.size());
    const float spacing_meters = chase.spacing_meters;
    if (spacing_meters <= 0.0f || agents.size() < 2u) {
        return spacing;
    }
    constexpr float kPi = 3.14159265358979f;
    struct Member {
        std::size_t index = 0;
        std::uint32_t net_id = 0;
        KernelVec3 position{0.0f, 0.0f, 0.0f};
        // Zero unless it is engaging something.
        std::uint32_t target = 0;
        float bearing = 0.0f;
        float rank = 0.0f;
    };
    std::vector<Member> members;
    members.reserve(agents.size());
    for (std::size_t index = 0; index < agents.size(); ++index) {
        const AgentRuntimeState& agent = *agents[index];
        const KernelServerEntityState* state = frame.actor_state(agent.net_id);
        if (state == nullptr) {
            continue;
        }
        Member member;
        member.index = index;
        member.net_id = agent.net_id;
        member.position = state->position;
        if (agent.sentry.target != 0u &&
            (agent.sentry.state == AgentSentryState::kAlert ||
             is_pursuing(agent.sentry.state))) {
            member.target = agent.sentry.target;
        }
        members.push_back(member);
    }

    // The push, from every other agent of the template, engaged or not.
    for (const Member& self : members) {
        if (self.target == 0u) {
            continue;
        }
        KernelVec2 push{0.0f, 0.0f};
        for (const Member& other : members) {
            if (other.net_id == self.net_id) {
                continue;
            }
            const float dx = self.position.x - other.position.x;
            const float dz = self.position.z - other.position.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance >= spacing_meters) {
                continue;
            }
            const float weight = 1.0f - distance / spacing_meters;
            if (distance > 1e-4f) {
                push.x += dx / distance * weight;
                push.y += dz / distance * weight;
            } else {
                // Standing on the same spot: split them by net id, so each
                // goes its own way and a replay goes the same way.
                push.x += self.net_id < other.net_id ? -weight : weight;
            }
        }
        if (std::sqrt(push.x * push.x + push.y * push.y) >= kSpacingPushDeadzone) {
            spacing[self.index].push = push;
        }
    }

    // The rings, for each target more than one of them is after. The nearest
    // fill the inner ring, between stop and resume so a member in its spot is
    // already holding and in reach; who does not fit waits on the next ring
    // out, a spacing further, and so on. Each ring's spots are a spacing apart
    // and spread round its members' own bearing by at most
    // kSpacingMaxOffTargetDegrees either way: a spot further round than that
    // cannot be walked to while keeping the target in sight.
    const float max_arc = kSpacingMaxOffTargetDegrees * kPi / 180.0f;
    const float inner_radius =
        0.5f * (chase.stop_distance_meters + chase.resume_distance_meters);
    std::vector<Member*> group;
    std::vector<Member*> ring;
    for (std::size_t first = 0; first < members.size(); ++first) {
        const std::uint32_t target = members[first].target;
        const KernelServerEntityState* target_state =
            target != 0u ? frame.actor_state(target) : nullptr;
        if (target_state == nullptr) {
            continue;
        }
        // Each group once: from its first member only.
        bool seen_before = false;
        for (std::size_t earlier = 0; earlier < first; ++earlier) {
            seen_before = seen_before || members[earlier].target == target;
        }
        if (seen_before) {
            continue;
        }
        group.clear();
        for (Member& member : members) {
            if (member.target == target) {
                const float dx = member.position.x - target_state->position.x;
                const float dz = member.position.z - target_state->position.z;
                member.bearing = std::atan2(dz, dx);
                // Who is nearer gets the inner ring. One already parked counts
                // a spacing nearer, so two closing at about the same range do
                // not trade rings every tick.
                member.rank = std::sqrt(dx * dx + dz * dz) -
                    (agents[member.index]->chase_holding ? spacing_meters : 0.0f);
                group.push_back(&member);
            }
        }
        if (group.size() < 2u) {
            continue;
        }
        std::sort(group.begin(), group.end(), [](const Member* lhs, const Member* rhs) {
            return lhs->rank != rhs->rank ? lhs->rank < rhs->rank
                                          : lhs->net_id < rhs->net_id;
        });
        std::size_t placed = 0;
        for (float radius = inner_radius; placed < group.size();
             radius += spacing_meters) {
            const float gap = spacing_meters < 2.0f * radius
                ? 2.0f * std::asin(spacing_meters / (2.0f * radius))
                : 2.0f * kPi;
            const std::size_t capacity = gap > 2.0f * max_arc
                ? 1u
                : static_cast<std::size_t>(2.0f * max_arc / gap) + 1u;
            ring.assign(
                group.begin() + static_cast<std::ptrdiff_t>(placed),
                group.begin() + static_cast<std::ptrdiff_t>(
                    std::min(group.size(), placed + capacity)));
            placed += ring.size();
            // Round the ring in bearing order, about its members' mean bearing:
            // unwrapped from the widest gap between them, so a ring straddling
            // +-180 degrees reads as one run, not two ends.
            std::sort(ring.begin(), ring.end(), [](const Member* lhs, const Member* rhs) {
                return lhs->bearing != rhs->bearing ? lhs->bearing < rhs->bearing
                                                    : lhs->net_id < rhs->net_id;
            });
            const std::size_t count = ring.size();
            std::size_t start = 0;
            float widest = -1.0f;
            for (std::size_t k = 0; k < count; ++k) {
                const float next = k + 1u < count ? ring[k + 1u]->bearing
                                                  : ring[0]->bearing + 2.0f * kPi;
                if (next - ring[k]->bearing > widest) {
                    widest = next - ring[k]->bearing;
                    start = (k + 1u) % count;
                }
            }
            float mean = 0.0f;
            for (std::size_t k = 0; k < count; ++k) {
                mean += ring[(start + k) % count]->bearing +
                    (start + k >= count ? 2.0f * kPi : 0.0f);
            }
            mean /= static_cast<float>(count);
            for (std::size_t k = 0; k < count; ++k) {
                const float angle = mean +
                    (static_cast<float>(k) - 0.5f * static_cast<float>(count - 1u)) * gap;
                ChaseSpacing& entry = spacing[ring[(start + k) % count]->index];
                entry.has_slot = true;
                entry.slot = KernelVec3{
                    target_state->position.x + radius * std::cos(angle),
                    target_state->position.y,
                    target_state->position.z + radius * std::sin(angle),
                };
            }
        }
    }
    return spacing;
}

// Hysteresis around the stop distance. Without the separate resume threshold an
// agent parked exactly on the boundary would start and stop every tick.
//
// With a ring slot it walks to the slot instead and holds there, on the same
// width of hysteresis measured from the slot; either way the push of
// `spacing` rides on top.
KernelVec2 chase_move(
    const AgentChaseTuning& chase,
    AgentRuntimeState* agent,
    const KernelVec3& target_position,
    const ChaseSpacing& spacing) {
    KernelVec2 move{0.0f, 0.0f};
    if (spacing.has_slot) {
        const float band = chase.resume_distance_meters - chase.stop_distance_meters;
        const float distance =
            agent_steering::horizontal_distance(agent->position, spacing.slot);
        if (agent->chase_holding) {
            if (distance >= band) {
                agent->chase_holding = false;
            }
        } else if (distance <= 0.5f * band) {
            agent->chase_holding = true;
        }
        if (!agent->chase_holding) {
            move = agent_steering::horizontal_move_toward(
                agent->position, spacing.slot, chase.input_magnitude);
        }
    } else {
        const float distance =
            agent_steering::horizontal_distance(agent->position, target_position);
        if (agent->chase_holding) {
            if (distance >= chase.resume_distance_meters) {
                agent->chase_holding = false;
            }
        } else if (distance <= chase.stop_distance_meters) {
            agent->chase_holding = true;
        }
        if (!agent->chase_holding) {
            move = agent_steering::horizontal_move_toward(
                agent->position, target_position, chase.input_magnitude);
        }
    }
    if (!spacing.has_slot && is_zero_move(spacing.push)) {
        return move;
    }
    move.x += spacing.push.x * chase.input_magnitude;
    move.y += spacing.push.y * chase.input_magnitude;
    float length = std::sqrt(move.x * move.x + move.y * move.y);
    if (length <= 1e-4f) {
        return KernelVec2{0.0f, 0.0f};
    }
    if (length > chase.input_magnitude) {
        move.x *= chase.input_magnitude / length;
        move.y *= chase.input_magnitude / length;
        length = chase.input_magnitude;
    }
    // Kept within sight of the target (kSpacingMaxOffTargetDegrees). A move
    // turned back to the edge of that closes on the target, so only one
    // heading off to the side, with room left to close, is turned back; one
    // heading away (the push, mostly) or inside the stop distance stands
    // instead. A legged agent cannot back off without turning its back.
    const float to_x = target_position.x - agent->position.x;
    const float to_z = target_position.z - agent->position.z;
    const float to_target = std::atan2(to_z, to_x);
    const float off_target = std::remainder(
        std::atan2(move.y, move.x) - to_target, 2.0f * 3.14159265358979f);
    const float max_off = kSpacingMaxOffTargetDegrees * 3.14159265358979f / 180.0f;
    if (std::abs(off_target) <= max_off) {
        return move;
    }
    if (std::abs(off_target) > 0.5f * 3.14159265358979f ||
        std::sqrt(to_x * to_x + to_z * to_z) <= chase.stop_distance_meters) {
        return KernelVec2{0.0f, 0.0f};
    }
    const float heading = to_target + (off_target > 0.0f ? max_off : -max_off);
    return KernelVec2{length * std::cos(heading), length * std::sin(heading)};
}

// A retreat that is getting somewhere covers this share of what its input
// asks for over each window; one that does not is up against something and
// fights from where it is.
constexpr std::uint32_t kRetreatStallWindowTicks = 15u;
constexpr float kRetreatStallFraction = 0.3f;
// Stops this short of the leash, so backing off is never what breaks it.
constexpr float kRetreatLeashMarginMeters = 0.5f;
// The ways it may back off, tried in order: straight away from the target,
// then 45 and 90 degrees either side. Past 90 it would be closing again.
constexpr std::array<float, 5> kRetreatHeadingDegrees{0.0f, 45.0f, -45.0f, 90.0f, -90.0f};
// How far ahead each heading has to stay on walkable ground.
constexpr float kRetreatProbeMeters = 2.0f;
// How far off the navmesh an agent may stand and still be matched to it.
constexpr float kRetreatNavmeshMatchMeters = 1.0f;

KernelVec2 rotated(const KernelVec2& move, float degrees) {
    const float radians = degrees * 3.14159265358979f / 180.0f;
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    return KernelVec2{move.x * c - move.y * s, move.x * s + move.y * c};
}

// Whether backing off along `move` keeps to walkable ground for the next
// kRetreatProbeMeters; unknown (no navmesh, or the agent off it) counts as
// clear, which is how R1 behaved before it could look.
bool retreat_heading_clear(
    const PatrolNavigation* navigation,
    const KernelVec3& position,
    const KernelVec2& move) {
    if (navigation == nullptr) {
        return true;
    }
    const float length = std::sqrt(move.x * move.x + move.y * move.y);
    if (length <= 1e-4f) {
        return true;
    }
    const KernelVec3 ahead{
        position.x + move.x / length * kRetreatProbeMeters,
        position.y,
        position.z + move.y / length * kRetreatProbeMeters,
    };
    return navigation->walkable_line(position, ahead, kRetreatNavmeshMatchMeters)
        .value_or(true);
}

// How long a turn_and_run may spend turning back before it gives the target
// up as out of sight: a legged giant at 45 degrees a second needs four
// seconds for the half turn.
constexpr std::uint32_t kRetreatTurnLimitTicks = 180u;

void end_retreat(const AgentRetreatTuning& retreat, AgentRuntimeState* agent) {
    agent->retreating = false;
    agent->retreat_turning = false;
    agent->retreat_turn_ticks = 0u;
    agent->retreat_ticks = 0u;
    agent->retreat_window_ticks = 0u;
    agent->retreat_cooldown_ticks = retreat.cooldown_ticks;
    // Hold where it ended: inside the hold band after a retreat that made its
    // distance, and too close to want to close after one that did not.
    agent->chase_holding = true;
}

// The end of the moving-away part of a retreat: a backpedal is over; a
// turn_and_run stops and turns back to face the target, which the caller does.
void finish_moving_away(const AgentRetreatTuning& retreat, AgentRuntimeState* agent) {
    if (retreat.style == AgentRetreatStyle::kTurnAndRun) {
        agent->retreat_turning = true;
        agent->retreat_turn_ticks = 0u;
        return;
    }
    end_retreat(retreat, agent);
}

// Backing off from a target that has come too close (chase.retreat,
// docs/AI_RANGED_RETREAT_PLAN.md). Called while pursuing, after chase_move:
// while moving away it replaces `move` with one away from the target. A
// backpedal keeps facing the target, which the caller does; a turn_and_run
// faces the way it goes, and once run_ticks are up turns back (the caller
// again). Never once a leap or charge is under way. A squad member backs off
// too, held to its squad's leash.
void update_retreat(
    const AgentRetreatTuning& retreat,
    float leash_meters,
    float step_meters,
    const PatrolNavigation* navigation,
    const KernelVec3& target_position,
    const KernelVec2& spacing_push,
    AgentRuntimeState* agent,
    KernelVec2* move) {
    if (retreat.trigger_distance_meters <= 0.0f) {
        return;
    }
    if (agent->leap_ticks_to_landing > 0u ||
        agent->charge_windup_ticks > 0u || agent->charge_ticks_left > 0u) {
        if (agent->retreating) end_retreat(retreat, agent);
        return;
    }
    const float distance =
        agent_steering::horizontal_distance(agent->position, target_position);
    if (!agent->retreating) {
        if (agent->retreat_cooldown_ticks > 0u ||
            distance >= retreat.trigger_distance_meters) {
            return;
        }
        agent->retreating = true;
        agent->retreat_turning = false;
        agent->retreat_turn_ticks = 0u;
        agent->retreat_ticks = 0u;
        agent->retreat_window_start = agent->position;
        agent->retreat_window_ticks = 0u;
        agent->retreat_heading = 0u;
    }
    if (agent->retreat_turning) {
        *move = KernelVec2{0.0f, 0.0f};
        return;
    }
    const bool run = retreat.style == AgentRetreatStyle::kTurnAndRun;
    bool done = (run ? agent->retreat_ticks >= retreat.run_ticks
                     : distance >= retreat.target_distance_meters ||
                           agent->retreat_ticks >= retreat.max_ticks) ||
        (leash_meters > 0.0f &&
         agent_steering::horizontal_distance(agent->position, agent->patrol.leash_anchor) >=
             leash_meters - kRetreatLeashMarginMeters);
    if (!done && agent->retreat_window_ticks >= kRetreatStallWindowTicks) {
        const float made_good = agent_steering::horizontal_distance(
            agent->position, agent->retreat_window_start);
        const float asked = step_meters * retreat.input_magnitude *
            static_cast<float>(agent->retreat_window_ticks);
        done = made_good < kRetreatStallFraction * asked;
        agent->retreat_window_start = agent->position;
        agent->retreat_window_ticks = 0u;
    }
    if (done) {
        finish_moving_away(retreat, agent);
        *move = KernelVec2{0.0f, 0.0f};
        return;
    }
    const KernelVec2 toward = agent_steering::horizontal_move_toward(
        agent->position, target_position, retreat.input_magnitude);
    const KernelVec2 away{-toward.x, -toward.y};
    // The heading it was on, if that is still clear; otherwise the first
    // clear one in order; none clear and it is cornered.
    std::size_t heading = kRetreatHeadingDegrees.size();
    if (agent->retreat_heading < kRetreatHeadingDegrees.size() &&
        retreat_heading_clear(
            navigation, agent->position,
            rotated(away, kRetreatHeadingDegrees[agent->retreat_heading]))) {
        heading = agent->retreat_heading;
    } else {
        for (std::size_t index = 0; index < kRetreatHeadingDegrees.size(); ++index) {
            if (retreat_heading_clear(
                    navigation, agent->position,
                    rotated(away, kRetreatHeadingDegrees[index]))) {
                heading = index;
                break;
            }
        }
    }
    if (heading == kRetreatHeadingDegrees.size()) {
        finish_moving_away(retreat, agent);
        *move = KernelVec2{0.0f, 0.0f};
        return;
    }
    agent->retreat_heading = static_cast<std::uint8_t>(heading);
    ++agent->retreat_ticks;
    ++agent->retreat_window_ticks;
    *move = rotated(away, kRetreatHeadingDegrees[heading]);
    // Others of its template too close (chase.spacing_meters) push it aside
    // as it goes, the way they do while it closes -- unless that would take it
    // somewhere the heading itself was not checked for.
    if (!is_zero_move(spacing_push)) {
        KernelVec2 pushed{
            move->x + spacing_push.x * retreat.input_magnitude,
            move->y + spacing_push.y * retreat.input_magnitude,
        };
        const float length = std::sqrt(pushed.x * pushed.x + pushed.y * pushed.y);
        if (length > retreat.input_magnitude) {
            pushed.x *= retreat.input_magnitude / length;
            pushed.y *= retreat.input_magnitude / length;
        }
        if (length > 1e-4f && retreat_heading_clear(navigation, agent->position, pushed)) {
            *move = pushed;
        }
    }
}

}  // namespace

AgentChaserController::AgentChaserController(AgentChaserConfig config)
    : config_(std::move(config)) {
    if (config_.weapon_skills.empty()) {
        AgentWeaponSkill weapon;
        weapon.executor.weapon_id = config_.sentry.weapon_id;
        weapon.executor.ballistic_aim = config_.sentry.ballistic_aim;
        weapon.executor.charge_ticks = config_.sentry.charge_ticks;
        weapon.range_max_meters = config_.chase.attack_range_meters;
        config_.weapon_skills.push_back(weapon);
    }
    for (std::size_t index = 0; index < config_.weapon_skills.size(); ++index) {
        if (config_.weapon_skills[index].executor.weapon_id ==
            config_.chase.leap.on_landing_weapon_id) {
            leap_weapon_skill_ = static_cast<std::uint8_t>(index);
            break;
        }
    }
}

void AgentChaserController::tick(
    KernelHandle* kernel,
    const PerceptionFrame& frame,
    const AgentBatch& agents,
    float delta_seconds,
    const PatrolNavigation* navigation) const {
    if (kernel == nullptr) {
        return;
    }

    const AgentSentryConfig& sentry_config = config_.sentry;
    const std::vector<ChaseSpacing> spacing =
        plan_chase_spacing(config_.chase, frame, agents);

    for (std::size_t agent_index = 0; agent_index < agents.size(); ++agent_index) {
        AgentRuntimeState& agent = *agents[agent_index];
        for (std::uint32_t& cooldown : agent.skill_cooldown_ticks) {
            if (cooldown > 0u) --cooldown;
        }
        if (agent.retreat_cooldown_ticks > 0u) {
            --agent.retreat_cooldown_ticks;
        }
        // Set where a pursuit with its target in sight runs the retreat; a
        // retreat that went a tick without one -- the target lost, the pursuit
        // over -- ends.
        bool retreat_ran = false;
        if (agent.weapon_skill >= config_.weapon_skills.size()) {
            agent.weapon_skill = 0;
        }
        SentryPerceptionSnapshot perception =
            AiPerceptionAdapter::build_sentry_snapshot(
                kernel, frame, agent.net_id);
        if (!perception.has_self_state) {
            continue;
        }
        const AgentBehavior& behavior = agent_behavior_for(agent);
        if (const std::uint32_t locked = behavior.locked_target(agent)) {
            AiPerceptionAdapter::focus_on_target(
                kernel, frame, agent.net_id, locked, &perception);
        }
        behavior.after_perception(agent, perception.has_visible_target);
        const KernelServerEntityState& entity_state = perception.self_state;
        // The weapon in hand went off (D14): its cooldown runs from here, and
        // the next shot is drawn afresh.
        if (entity_state.action.action_instance_id != 0u &&
            entity_state.action.action_instance_id != agent.seen_action_instance_id) {
            agent.seen_action_instance_id = entity_state.action.action_instance_id;
            if (entity_state.is_reloading == 0u &&
                entity_state.action.phase != KernelActionPhase_None) {
                agent.skill_cooldown_ticks[agent.weapon_skill] =
                    config_.weapon_skills[agent.weapon_skill].cooldown_ticks;
                agent.choose_weapon = true;
            }
        }

        agent.position = entity_state.position;
        agent.hp = entity_state.hp;
        agent.velocity = agent_steering::zero_vec3();
        agent.animation_state = sentry_config.animation_idle;
        agent.sentry.self_id = agent.net_id;
        // A leap counts down to its landing. The kernel arms it on the tick
        // after it is asked, so from then on the flag says it is in the air;
        // without the flag it was refused or something ended it, and there is
        // no landing left to time a swing against.
        if (agent.leap_ticks_to_landing > 0u) {
            --agent.leap_ticks_to_landing;
            if ((entity_state.visual_flags & KERNEL_VISUAL_FLAG_LEAPING) == 0u) {
                agent.leap_ticks_to_landing = 0u;
            }
        }
        if (agent.leap_cooldown_ticks > 0u) {
            --agent.leap_cooldown_ticks;
        }
        // A charge counts down the same way: armed the tick after it is
        // asked, and over -- a hit, a wall, a stagger -- once the flag is gone.
        if (agent.charge_ticks_left > 0u) {
            --agent.charge_ticks_left;
            if ((entity_state.visual_flags & KERNEL_VISUAL_FLAG_CHARGING) == 0u) {
                agent.charge_ticks_left = 0u;
            }
        }
        if (agent.charge_cooldown_ticks > 0u) {
            --agent.charge_cooldown_ticks;
        }

        AgentBehaviorStep step{config_, perception, entity_state};
        step.desired_rotation = entity_state.rotation;
        KernelQuat& desired_rotation = step.desired_rotation;
        bool& should_update_rotation = step.should_update_rotation;
        KernelVec2& move = step.move;
        bool submitted_input = false;

        const bool has_visible_target = perception.has_visible_target;

        if (has_visible_target) {
            if (agent.sentry.target != perception.target_id) {
                agent.sentry.ballistic_retry_ticks = 0;
            }
            agent.sentry.target = perception.target_id;
            agent.sentry.lost_target_ticks = 0;
            // Feet, not the aim point: Search walks to it.
            const KernelServerEntityState* seen =
                frame.actor_state(perception.target_id);
            agent.sentry.attention_point =
                seen != nullptr ? seen->position : perception.target_position;
            agent.sentry.has_attention_point = true;
        } else {
            ++agent.sentry.lost_target_ticks;
            // The hold is about a target in sight; Search walks to where it
            // was last seen instead.
            agent.chase_holding = false;
        }

        // A hit (AI plan D8), before the states run. Idle or searching, it
        // alerts -- the short alert -- with its attention on where the hit came
        // from; already alert or pursuing something it cannot see, its
        // attention moves there (a Search then heads for it); something it can
        // see keeps its attention. Returning on a leash it only turns its head:
        // a leash it could be shot back out of would never bring it home.
        bool face_damage_source = false;
        if (perception.damaged) {
            const AgentSentryState state = agent.sentry.state;
            if (state == AgentSentryState::kIdle || state == AgentSentryState::kSearch ||
                state == AgentSentryState::kSeek) {
                if (state != AgentSentryState::kSearch) {
                    agent.patrol.leash_anchor = agent.position;
                }
                agent_steering::transition_to(&agent, AgentSentryState::kAlert);
                agent.sentry.alerted_by_damage = true;
            }
            if (agent.sentry.state == AgentSentryState::kReturn) {
                face_damage_source = true;
            } else if (!has_visible_target) {
                agent.sentry.attention_point = perception.damage_source;
                agent.sentry.has_attention_point = true;
            }
        }

        // Breaking off is a decision about the squad, not about the target, so
        // it is made before the pursuit states get to look at what they can
        // see. Without it the leash could only be enforced on a tick where
        // vision happened to break.
        if (leash_exceeded(behavior.leash_meters(config_), agent) &&
            is_engaged(agent.sentry.state)) {
            agent.sentry.target = 0;
            agent.sentry.has_attention_point = false;
            agent.chase_holding = false;
            agent_steering::transition_to(&agent, AgentSentryState::kReturn);
        }

        // Ordered ahead of kIdle so that an agent which arrives back on its
        // route resumes walking it on the same tick, the way kIdle -> kAlert
        // already reaches the alert branch in the tick it transitions.
        if (agent.sentry.state == AgentSentryState::kReturn) {
            behavior.on_return(agent, step);
        }

        if (agent.sentry.state == AgentSentryState::kIdle) {
            if (has_visible_target) {
                // Recorded before the chase starts, so the leash measures the
                // pursuit rather than the ground the squad has since covered.
                agent.patrol.leash_anchor = agent.position;
                agent.sentry.alerted_by_damage = false;
                agent_steering::transition_to(&agent, AgentSentryState::kAlert);
            } else {
                behavior.on_idle(agent, step);
            }
        }

        // A hunter walking to its target's last broad-phase position, corner by
        // corner of the path the manager found. Seeing anything hostile
        // alerts it the way Idle would, on this same tick.
        if (agent.sentry.state == AgentSentryState::kSeek) {
            if (has_visible_target) {
                agent.patrol.leash_anchor = agent.position;
                agent.sentry.alerted_by_damage = false;
                agent_steering::transition_to(&agent, AgentSentryState::kAlert);
            } else {
                while (agent.seek_next < agent.seek_path.size() &&
                       agent_steering::horizontal_distance(
                           agent.position, agent.seek_path[agent.seek_next]) <=
                           kSearchArrivalMeters) {
                    ++agent.seek_next;
                }
                if (agent.seek_next < agent.seek_path.size()) {
                    const KernelVec3& corner = agent.seek_path[agent.seek_next];
                    move = agent_steering::horizontal_move_toward(
                        agent.position, corner, config_.chase.input_magnitude);
                    should_update_rotation =
                        agent_steering::facing_rotation_from_vision_toward(
                            agent.position,
                            corner,
                            perception.vision_forward,
                            entity_state.rotation,
                            &desired_rotation) ||
                        should_update_rotation;
                } else {
                    // There, or nothing to go to: look around until the next
                    // broad-phase read moves it on.
                    should_update_rotation = agent_steering::update_patrol_facing(
                        sentry_config.patrol_rotation_interval_ticks,
                        sentry_config.patrol_rotation_min_degrees,
                        sentry_config.patrol_rotation_max_degrees,
                        &agent,
                        entity_state.rotation,
                        &desired_rotation);
                }
            }
        }

        if (agent.sentry.state == AgentSentryState::kAlert) {
            if (has_visible_target) {
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        perception.target_position,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
                move = chase_move(
                    config_.chase, &agent, perception.target_position,
                    spacing[agent_index]);
                ++agent.sentry.state_ticks;
                if (agent.sentry.state_ticks >=
                    (agent.sentry.alerted_by_damage ? sentry_config.alert_on_damage_ticks
                                                    : sentry_config.alert_ticks)) {
                    // The pursuit below sorts Chasing from Attack this tick.
                    agent_steering::transition_to(
                        &agent, AgentSentryState::kChasing);
                }
            } else if (agent.sentry.lost_target_ticks >=
                       sentry_config.forget_ticks) {
                // Lost before the alert ran out: no Search, so a target that
                // ducks in and out of view cannot skip the warning. It waits
                // here, as it always has, and gives up.
                agent.sentry.target = 0;
                agent.sentry.has_attention_point = false;
                agent_steering::transition_to(
                    &agent, behavior.on_target_forgotten(agent));
            } else if (agent.sentry.has_attention_point) {
                // Nothing in sight: stop and face where its attention is --
                // the last place it saw something, or where a hit came from.
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        agent.sentry.attention_point,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
            }
        }

        // A wind-up is for this target in this pursuit: losing either drops it.
        // So does being knocked back or staggered out of it.
        if (agent.charge_windup_ticks > 0u &&
            (!is_pursuing(agent.sentry.state) ||
             !has_visible_target ||
             (entity_state.visual_flags & (KERNEL_VISUAL_FLAG_KNOCKED_BACK |
                                           KERNEL_VISUAL_FLAG_STAGGERED)) != 0u)) {
            agent.charge_windup_ticks = 0u;
        }

        const KernelServerEntityState* target_state =
            frame.actor_state(agent.sentry.target);
        if (is_pursuing(agent.sentry.state) &&
            has_visible_target && agent.charge_ticks_left > 0u) {
            // The kernel runs the charge: no steering, no turning, no swing.
            // The tail still sends the one input, with no move in it.
            agent.sentry.lost_target_ticks = 0;
            agent.sentry.state = AgentSentryState::kAttack;
        } else if (is_pursuing(agent.sentry.state) &&
                   has_visible_target && agent.charge_windup_ticks > 0u) {
            // Winding up: rooted, turning to keep the target in front.
            agent.sentry.lost_target_ticks = 0;
            agent.sentry.state = AgentSentryState::kAttack;
            should_update_rotation =
                agent_steering::facing_rotation_from_vision_toward(
                    agent.position,
                    perception.target_position,
                    perception.vision_forward,
                    entity_state.rotation,
                    &desired_rotation) ||
                should_update_rotation;
            if (--agent.charge_windup_ticks == 0u && target_state != nullptr) {
                release_charge(kernel, config_, &agent, target_state->position);
            }
        } else if (is_pursuing(agent.sentry.state) && agent.retreating &&
                   config_.chase.retreat.style == AgentRetreatStyle::kTurnAndRun) {
            // Running from the target with its back to it, then turning back
            // (§13). Out of sight is expected here, so the target is followed
            // by id rather than dropped into Search; a hit only moves its
            // attention, and it keeps running (§13 decision 1). It holds its
            // fire throughout.
            retreat_ran = true;
            agent.sentry.lost_target_ticks = 0;
            const KernelServerEntityState* fled_from = frame.actor_state(agent.sentry.target);
            if (fled_from == nullptr) {
                end_retreat(config_.chase.retreat, &agent);
                agent_steering::transition_to(&agent, AgentSentryState::kSearch);
            } else {
                const KernelVec3 from = has_visible_target
                    ? perception.target_position
                    : fled_from->position;
                update_retreat(
                    config_.chase.retreat,
                    behavior.leash_meters(config_),
                    sentry_config.move_speed_meters_per_second * delta_seconds,
                    config_.hovers ? nullptr : navigation,
                    from,
                    spacing[agent_index].push,
                    &agent,
                    &move);
                if (agent.retreating && agent.retreat_turning) {
                    move = KernelVec2{0.0f, 0.0f};
                    should_update_rotation =
                        agent_steering::facing_rotation_from_vision_toward(
                            agent.position, from, perception.vision_forward,
                            entity_state.rotation, &desired_rotation) ||
                        should_update_rotation;
                    ++agent.retreat_turn_ticks;
                    // Facing it again: the pursuit takes over from the next
                    // tick. Still not, past the limit: lost, as any target
                    // that is out of sight is.
                    if (has_visible_target ||
                        agent.retreat_turn_ticks >= kRetreatTurnLimitTicks) {
                        end_retreat(config_.chase.retreat, &agent);
                    }
                } else if (agent.retreating && !is_zero_move(move)) {
                    should_update_rotation =
                        agent_steering::facing_rotation_from_vision_toward(
                            agent.position,
                            KernelVec3{agent.position.x + move.x, agent.position.y,
                                       agent.position.z + move.y},
                            perception.vision_forward, entity_state.rotation,
                            &desired_rotation) ||
                        should_update_rotation;
                }
                agent.sentry.state = AgentSentryState::kChasing;
            }
        } else if (is_pursuing(agent.sentry.state)) {
            if (has_visible_target) {
                agent.sentry.lost_target_ticks = 0;
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        perception.target_position,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
                move = chase_move(
                    config_.chase, &agent, perception.target_position,
                    spacing[agent_index]);
                update_retreat(
                    config_.chase.retreat,
                    behavior.leash_meters(config_),
                    sentry_config.move_speed_meters_per_second * delta_seconds,
                    config_.hovers ? nullptr : navigation,
                    perception.target_position,
                    spacing[agent_index].push,
                    &agent,
                    &move);
                retreat_ran = true;
                // A charge is tried ahead of a leap: winding one up roots the
                // agent from this tick, and a leap must not carry it off.
                // Neither starts while it backs away.
                if (agent.retreating) {
                } else if (target_state != nullptr &&
                    try_start_charge(
                        config_, &agent, entity_state, target_state->position)) {
                    move = KernelVec2{0.0f, 0.0f};
                } else if (target_state != nullptr) {
                    (void)try_start_leap(
                        kernel, config_, &agent, entity_state,
                        target_state->position);
                }
                // Seeing a target is not the same as being able to hit one.
                // Not attacking leaves submitted_input false, so the tail of
                // the loop still sends exactly one input carrying `move` --
                // the agent keeps closing, it just does not swing yet.
                //
                // In a leap, range is the landing's business: it comes down
                // inside it, so the swing goes by how soon that is instead.
                // Which weapon (D14): the leap's own swing while one is in the
                // air, otherwise whichever skill the agent draws.
                const float distance = agent_steering::horizontal_distance(
                    agent.position, perception.target_position);
                if (agent.leap_ticks_to_landing > 0u) {
                    agent.weapon_skill = leap_weapon_skill_;
                } else {
                    choose_weapon_skill(
                        config_.weapon_skills, entity_state, distance, &agent);
                }
                const AgentWeaponSkill& weapon =
                    config_.weapon_skills[agent.weapon_skill];
                // Whether to fire: the weapon in hand reaches and is off its
                // cooldown. Whether it is attacking at all (D4): anything it
                // carries reaches, cooling down or not -- a rocket that has
                // just gone off is still an attack in progress.
                const bool target_within_attack_range =
                    agent.leap_ticks_to_landing > 0u
                    ? agent.leap_ticks_to_landing <=
                        config_.chase.leap.attack_lead_ticks
                    : in_reach(weapon, distance) &&
                        agent.skill_cooldown_ticks[agent.weapon_skill] == 0u;
                const bool something_reaches =
                    agent.leap_ticks_to_landing > 0u
                    ? target_within_attack_range
                    : std::any_of(
                          config_.weapon_skills.begin(), config_.weapon_skills.end(),
                          [distance](const AgentWeaponSkill& skill) {
                              return in_reach(skill, distance);
                          });
                // Set directly, not through transition_to: flipping between
                // the two must not reset the pursuit's counters. A wind-up
                // started this tick is an attack already.
                agent.sentry.state =
                    something_reaches || agent.charge_windup_ticks > 0u
                    ? AgentSentryState::kAttack
                    : AgentSentryState::kChasing;
                if (agent.sentry.ballistic_retry_ticks > 0) {
                    --agent.sentry.ballistic_retry_ticks;
                } else if (agent.charge_windup_ticks > 0u) {
                    // Wound up this tick: no swing on top of it.
                } else if (target_within_attack_range &&
                           (!agent.retreating ||
                            config_.chase.retreat.fire_while_retreating)) {
                    ai::ScopedIntent intent;
                    intent.scope = ai::IntentScope::kActor;
                    intent.type = "AttackTarget";
                    intent.subject = agent.net_id;
                    intent.params["target_id"] = agent.sentry.target;
                    // The move rides on the attack input: a separate movement
                    // input in the same tick would carry a higher input_seq and
                    // the movement solver would take that one instead.
                    const ActorIntentExecutionResult execution =
                        ActorIntentExecutor(weapon.executor).execute(
                            kernel,
                            &agent,
                            intent,
                            perception,
                            move);
                    if (execution.ballistic_solution_unavailable) {
                        agent.sentry.ballistic_retry_ticks =
                            sentry_config.ballistic_retry_cooldown_ticks;
                    }
                    submitted_input = execution.submitted_input;
                }
            } else {
                agent_steering::transition_to(&agent, AgentSentryState::kSearch);
            }
        }

        // After the pursuit, so an agent that loses sight starts walking to
        // where it last saw the target on that same tick. Seeing it again
        // resumes the pursuit from the next one.
        if (agent.sentry.state == AgentSentryState::kSearch) {
            if (has_visible_target) {
                agent_steering::transition_to(&agent, AgentSentryState::kChasing);
            } else if (agent.sentry.lost_target_ticks >=
                           sentry_config.forget_ticks ||
                       !agent.sentry.has_attention_point) {
                agent.sentry.target = 0;
                agent.sentry.has_attention_point = false;
                agent_steering::transition_to(
                    &agent, behavior.on_target_forgotten(agent));
            } else if (agent_steering::horizontal_distance(
                           agent.position, agent.sentry.attention_point) >
                       kSearchArrivalMeters) {
                move = agent_steering::horizontal_move_toward(
                    agent.position,
                    agent.sentry.attention_point,
                    config_.chase.input_magnitude);
                should_update_rotation =
                    agent_steering::facing_rotation_from_vision_toward(
                        agent.position,
                        agent.sentry.attention_point,
                        perception.vision_forward,
                        entity_state.rotation,
                        &desired_rotation) ||
                    should_update_rotation;
            } else {
                // Arrived and nothing there: look around until it forgets.
                should_update_rotation = agent_steering::update_patrol_facing(
                    sentry_config.patrol_rotation_interval_ticks,
                    sentry_config.patrol_rotation_min_degrees,
                    sentry_config.patrol_rotation_max_degrees,
                    &agent,
                    entity_state.rotation,
                    &desired_rotation);
            }
        }

        if (face_damage_source) {
            should_update_rotation =
                agent_steering::facing_rotation_from_vision_toward(
                    agent.position,
                    perception.damage_source,
                    perception.vision_forward,
                    entity_state.rotation,
                    &desired_rotation) ||
                should_update_rotation;
        }

        if (!is_zero_move(move)) {
            agent.animation_state = sentry_config.animation_attack;
            agent.velocity = KernelVec3{
                move.x * sentry_config.move_speed_meters_per_second,
                entity_state.velocity.y,
                move.y * sentry_config.move_speed_meters_per_second,
            };
        }
        if (agent.charge_windup_ticks > 0u) {
            agent.animation_state = config_.chase.charge.has_windup_animation
                ? config_.chase.charge.windup_animation
                : sentry_config.animation_idle;
        } else if (agent.charge_ticks_left > 0u) {
            agent.animation_state = sentry_config.animation_attack;
        }

        if (agent.retreating && !retreat_ran) {
            end_retreat(config_.chase.retreat, &agent);
        }

        // Exactly one input per tick, always. An agent that submits none keeps
        // the horizontal velocity it already had, because the movement solver
        // only zeroes uncommanded *players*.
        if (!submitted_input) {
            KernelPlayerInput input{};
            input.input_seq = agent.next_input_seq;
            input.move = move;
            // Must match what the executor sends, or an in-flight action reads
            // this as a weapon change and cancels itself.
            const ActorIntentExecutorConfig& in_hand =
                config_.weapon_skills[agent.weapon_skill].executor;
            input.selected_weapon = static_cast<std::uint8_t>(in_hand.weapon_id);
            // And so must the aim. A zero aim_dir is not "no opinion": the
            // kernel reads it as aiming down world +X and replicates that, so
            // every tick the executor declined to fire on -- reloading, waiting
            // out a ballistic retry, closing the last metres into range -- used
            // to turn the agent away from the target it is shooting at.
            input.aim_dir = ActorIntentExecutor(in_hand).input_aim_direction(
                kernel, agent.net_id, perception);
            if (Kernel_ServerEnqueueEntityInput(
                    kernel,
                    KernelCommandSource_AI,
                    agent.net_id,
                    &input)) {
                ++agent.next_input_seq;
            }
        }

        agent.target_player_net_id = agent.sentry.target;
        if (should_update_rotation) {
            Kernel_ServerEnqueueEntityTransform(
                kernel,
                KernelCommandSource_AI,
                agent.net_id,
                &entity_state.position,
                &desired_rotation);
        }
        Kernel_ServerEnqueueEntityState(
            kernel,
            KernelCommandSource_AI,
            agent.net_id,
            agent.animation_state,
            0);
    }
}

}  // namespace network_example::game_server
