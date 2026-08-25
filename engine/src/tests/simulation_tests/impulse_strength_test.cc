// apply_impulse's two strength forms.
//
// The scalar form is what apply_impulse has always meant: delta =
// normalize(direction) * strength, one number, isotropic. The list form
// [horizontal, vertical] reads both numbers as metres per second and treats
// vertical as an ABSOLUTE signed Y increment, not a scale on direction.y.
//
// That distinction is the whole point. An area effect hands apply_impulse a
// radial direction -- blast centre to hit point -- so for a blast at the
// target's own height direction.y is ~0. A per-component scale would multiply
// that zero and could never launch anything upward, however large the vertical
// number was authored. These tests pin both readings against each other.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "kernel/src/kernel.h"
#include "simulation/public/action_graph.h"
#include "simulation/src/systems.h"

namespace {

using namespace network_example;

void require_impl(bool condition, const char* text, int line) {
    if (!condition) {
        std::fprintf(stderr, "impulse_strength_test:%d: %s\n", line, text);
        std::abort();
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

bool nearly(float lhs, float rhs) {
    return std::fabs(lhs - rhs) < 0.001f;
}

// A blast levelled with its target: the radial direction has a token amount of
// Y and no more, which is what the standing area-effect test pins.
const glm::vec3 kLevelRadial = glm::normalize(glm::vec3{1.0f, 0.02f, 0.0f});

CompiledActionGraphBinding impulse_binding(
    std::uint32_t strength_mode,
    float horizontal,
    float vertical,
    const glm::vec3& direction) {
    CompiledActionGraphBinding binding;
    binding.event_type = TriggerEventType::kActivated;
    binding.graph.id = "apply_impulse";
    binding.graph.parameters = {
        {"target", std::monostate{}},
        {"strength", horizontal},
        {"direction", direction},
    };
    ActionApplyImpulseDefinition impulse;
    impulse.target_parameter = "target";
    impulse.strength_parameter = "strength";
    impulse.direction_parameter = "direction";
    impulse.collision_mask = KERNEL_COLLISION_MASK_ACTOR;
    impulse.strength_mode = strength_mode;
    impulse.vertical_strength = vertical;
    binding.graph.actions = {impulse};
    binding.parameters = {
        {"target", EntityRefExpression{EntityRefSource::kEventTarget}},
    };
    return binding;
}

struct Fired {
    bool ok = false;
    glm::vec3 velocity{0.0f};
};

Fired fire(
    std::uint32_t strength_mode,
    float horizontal,
    float vertical,
    const glm::vec3& direction,
    float resistance = 0.0f) {
    KernelEngine engine(KernelConfig{});
    World& world = engine.simulation_world();
    const NetId source = world.spawn_player(1, glm::vec3{0.0f});
    const NetId target = world.spawn_enemy(glm::vec3{5.0f, 0.0f, 0.0f});
    const entt::entity target_entity = *world.find_entity(target);
    if (resistance > 0.0f) {
        world.registry().emplace<ImpulseResistance>(
            target_entity, ImpulseResistance{resistance});
    }

    const TriggerEvent event{
        TriggerEventType::kActivated, source, source, target};
    ActionExecutionProvenance provenance;
    provenance.request_id = 1;
    provenance.server_tick = engine.current_tick();
    provenance.instigator = source;
    provenance.owner_peer = 1u;
    std::vector<ActionGraphCommand> commands;
    Fired result;
    if (!evaluate_action_graph(
            impulse_binding(strength_mode, horizontal, vertical, direction),
            source,
            event,
            provenance,
            &commands,
            nullptr)) {
        return result;
    }
    result.ok = execute_action_graph_command_batch(
        engine,
        ActionGraphCommandBatch{event, provenance, 1u, std::move(commands)},
        0u);
    if (result.ok) {
        result.velocity =
            world.registry().get_or_emplace<Velocity>(target_entity).linear;
    }
    return result;
}

// Regression guard for every template authored before the list form existed.
void scalar_strength_still_scales_the_whole_direction() {
    const Fired axis = fire(
        KERNEL_IMPULSE_STRENGTH_MODE_RADIAL, 12.0f, 0.0f,
        glm::vec3{1.0f, 0.0f, 0.0f});
    require(axis.ok);
    require(nearly(axis.velocity.x, 12.0f));
    require(nearly(axis.velocity.y, 0.0f));
    require(nearly(axis.velocity.z, 0.0f));

    // Isotropic on every axis, including Y, which is what "radial" means.
    const glm::vec3 diagonal = glm::normalize(glm::vec3{1.0f, 1.0f, 1.0f});
    const Fired skew = fire(
        KERNEL_IMPULSE_STRENGTH_MODE_RADIAL, 12.0f, 0.0f, diagonal);
    require(skew.ok);
    require(nearly(skew.velocity.x, diagonal.x * 12.0f));
    require(nearly(skew.velocity.y, diagonal.y * 12.0f));
    require(nearly(skew.velocity.z, diagonal.z * 12.0f));

    // The vertical field is inert in radial mode: an ABI field arriving on an
    // old-form action must not silently change what that action does.
    const Fired ignored = fire(
        KERNEL_IMPULSE_STRENGTH_MODE_RADIAL, 12.0f, 99.0f,
        glm::vec3{1.0f, 0.0f, 0.0f});
    require(ignored.ok);
    require(nearly(ignored.velocity.y, 0.0f));
}

// The case the split form exists for.
void split_strength_lifts_out_of_a_level_blast() {
    // What the scalar form can do with the same radial direction: essentially
    // nothing vertical, because it is scaling a ~zero.
    const Fired scaled = fire(
        KERNEL_IMPULSE_STRENGTH_MODE_RADIAL, 8.0f, 0.0f, kLevelRadial);
    require(scaled.ok);
    require(scaled.velocity.y < 0.2f);

    // The split form reads its second number straight off the page: push
    // horizontally at 8 m/s, lift at 4 m/s.
    const Fired split = fire(
        KERNEL_IMPULSE_STRENGTH_MODE_SPLIT, 8.0f, 4.0f, kLevelRadial);
    require(split.ok);
    require(nearly(split.velocity.x, kLevelRadial.x * 8.0f));
    require(nearly(split.velocity.z, kLevelRadial.z * 8.0f));
    require(nearly(split.velocity.y, 4.0f));

    // Signed, so the author picks up or down rather than inheriting the
    // radial sign.
    const Fired slam = fire(
        KERNEL_IMPULSE_STRENGTH_MODE_SPLIT, 8.0f, -4.0f, kLevelRadial);
    require(slam.ok);
    require(nearly(slam.velocity.y, -4.0f));
}

// A launcher: all vertical, no horizontal. Unauthorable in the scalar form,
// where a zero strength is rejected outright.
void split_strength_allows_a_pure_vertical_launch() {
    const Fired launch = fire(
        KERNEL_IMPULSE_STRENGTH_MODE_SPLIT, 0.0f, 10.0f, kLevelRadial);
    require(launch.ok);
    require(nearly(launch.velocity.x, 0.0f));
    require(nearly(launch.velocity.z, 0.0f));
    require(nearly(launch.velocity.y, 10.0f));

    // Zero on both axes is not an impulse at all, and is refused.
    const Fired nothing = fire(
        KERNEL_IMPULSE_STRENGTH_MODE_SPLIT, 0.0f, 0.0f, kLevelRadial);
    require(!nothing.ok);
}

// impulse_resistance weighs the larger of the two authored axes, so a heavy
// target can still be launched by something that cannot shove it sideways.
void split_strength_is_weighed_on_its_larger_axis() {
    const Fired shrugged = fire(
        KERNEL_IMPULSE_STRENGTH_MODE_SPLIT, 8.0f, 4.0f, kLevelRadial, 10.0f);
    require(shrugged.ok);
    require(nearly(shrugged.velocity.x, 0.0f));
    require(nearly(shrugged.velocity.y, 0.0f));

    const Fired launched = fire(
        KERNEL_IMPULSE_STRENGTH_MODE_SPLIT, 8.0f, 20.0f, kLevelRadial, 10.0f);
    require(launched.ok);
    require(nearly(launched.velocity.y, 20.0f));

    // And radial mode weighs exactly what it always did.
    const Fired radial_under = fire(
        KERNEL_IMPULSE_STRENGTH_MODE_RADIAL, 8.0f, 0.0f, kLevelRadial, 10.0f);
    require(radial_under.ok);
    require(nearly(radial_under.velocity.x, 0.0f));
}

}  // namespace

int main() {
    scalar_strength_still_scales_the_whole_direction();
    split_strength_lifts_out_of_a_level_blast();
    split_strength_allows_a_pure_vertical_launch();
    split_strength_is_weighed_on_its_larger_axis();
    return 0;
}
