// What the rewound world is shaped like.
//
// Lag compensation tests a shot against HistoryFrame volumes rather than
// against the physics world, so the geometry here has to agree with the
// geometry there. HitVolumeSnapshot has carried a rotation from the start; this
// pins that it is actually used, because testing the world-axis bounds instead
// is close enough for an upright body box and badly wrong for anything long
// lying at an angle -- which is what a rig's leg is.

#include "sync/public/history_buffer.h"
#include "world/public/components.h"
#include "world/public/world.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

void require_impl(bool condition, int line) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d\n", line);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__)

network_example::HistoryFrame frame_with(
    const network_example::HitVolumeSnapshot& volume) {
    network_example::HistoryFrame frame;
    frame.server_tick = 1;
    frame.valid = true;
    frame.volumes.push_back(volume);
    return frame;
}

}  // namespace

int main() {
    // A long thin box lying at 45 degrees in the XZ plane. Its own extents are
    // 0.5 x 0.5 x 10, so along the world axes it spans roughly 7 m either way --
    // and the corner of that span is empty.
    network_example::HitVolumeSnapshot slab;
    slab.net_id = 42u;
    slab.center = glm::vec3{0.0f, 0.0f, 0.0f};
    slab.half_extents = glm::vec3{0.5f, 0.5f, 10.0f};
    slab.rotation = glm::angleAxis(
        glm::radians(45.0f), glm::vec3{0.0f, 1.0f, 0.0f});
    slab.alive = 1u;
    const network_example::HistoryFrame frame = frame_with(slab);

    // Straight down the slab's own long axis: a hit either way.
    {
        network_example::HistoricalHitResult hit;
        const glm::vec3 along = glm::normalize(glm::vec3{1.0f, 0.0f, 1.0f});
        require(network_example::raycast_history_frame(
            frame, along * -30.0f, along, 100.0f, 0u, &hit));
        require(hit.net_id == 42u);
    }

    // Inside the axis-aligned bound but outside the slab, heading further into
    // that empty corner. Testing the bound would report a hit at zero distance
    // because the ray starts within it; the box is nowhere near.
    {
        network_example::HistoricalHitResult hit;
        require(!network_example::raycast_history_frame(
            frame,
            glm::vec3{6.5f, 0.0f, -6.5f},
            glm::normalize(glm::vec3{1.0f, 0.0f, -1.0f}),
            5.0f,
            0u,
            &hit));
    }

    // An unrotated box still behaves exactly as it did.
    {
        network_example::HitVolumeSnapshot upright = slab;
        upright.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        const network_example::HistoryFrame upright_frame = frame_with(upright);
        network_example::HistoricalHitResult hit;
        require(network_example::raycast_history_frame(
            upright_frame,
            glm::vec3{0.0f, 0.0f, -30.0f},
            glm::vec3{0.0f, 0.0f, 1.0f},
            100.0f,
            0u,
            &hit));
        // The reported distance is the world distance to the near face.
        require(std::abs(hit.distance - 20.0f) < 0.001f);
    }

    // The shooter's own volume is skipped, and a dead one never answers.
    {
        network_example::HistoricalHitResult hit;
        const glm::vec3 along = glm::normalize(glm::vec3{1.0f, 0.0f, 1.0f});
        require(!network_example::raycast_history_frame(
            frame, along * -30.0f, along, 100.0f, 42u, &hit));

        network_example::HitVolumeSnapshot dead = slab;
        dead.alive = 0u;
        const network_example::HistoryFrame dead_frame = frame_with(dead);
        require(!network_example::raycast_history_frame(
            dead_frame, along * -30.0f, along, 100.0f, 0u, &hit));
    }

    // Limbs are in the frame but opt-in: a caller that has not asked for them
    // must see exactly what it saw before they were captured.
    {
        network_example::HitVolumeSnapshot limb = slab;
        limb.is_limb = 1u;
        const network_example::HistoryFrame limb_frame = frame_with(limb);
        const glm::vec3 along = glm::normalize(glm::vec3{1.0f, 0.0f, 1.0f});
        network_example::HistoricalHitResult hit;
        require(!network_example::raycast_history_frame(
            limb_frame, along * -30.0f, along, 100.0f, 0u, &hit));
        require(network_example::raycast_history_frame(
            limb_frame, along * -30.0f, along, 100.0f, 0u, &hit, true));
        require(hit.net_id == 42u);
        require(hit.volume.is_limb == 1u);
        // And the multiplier rides along, so a rewound leg costs what a live
        // one does.
        require(hit.volume.hit_zone == network_example::kHitZoneUnscaled);
    }

    // A hitbox in the same frame answers either way.
    {
        network_example::HitVolumeSnapshot body = slab;
        body.rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
        network_example::HistoryFrame mixed = frame_with(body);
        network_example::HitVolumeSnapshot limb = slab;
        limb.net_id = 43u;
        limb.is_limb = 1u;
        mixed.volumes.push_back(limb);
        network_example::HistoricalHitResult hit;
        require(network_example::raycast_history_frame(
            mixed,
            glm::vec3{0.0f, 0.0f, -30.0f},
            glm::vec3{0.0f, 0.0f, 1.0f},
            100.0f,
            0u,
            &hit));
        require(hit.net_id == 42u);
    }

    // Two layers (ABI 106). An actor with bounds is hit on its bones and only
    // on them: the bounds box is a gate and never a hit, and include_limbs
    // does not decide whether those bones are tested.
    {
        // A 12 m-wide body standing on two legs 8 m apart, the way the
        // quadruped's bounds and two of its legs are laid out.
        network_example::HitVolumeSnapshot bounds{};
        bounds.net_id = 50u;
        bounds.center = glm::vec3{0.0f, 12.0f, 0.0f};
        bounds.half_extents = glm::vec3{12.0f, 14.0f, 12.0f};
        bounds.alive = 1u;
        bounds.is_bounds = 1u;
        network_example::HitVolumeSnapshot left_leg{};
        left_leg.net_id = 50u;
        left_leg.center = glm::vec3{-4.0f, 5.0f, 0.0f};
        left_leg.half_extents = glm::vec3{1.0f, 5.0f, 1.0f};
        left_leg.hit_zone = 50u;
        left_leg.alive = 1u;
        left_leg.is_limb = 1u;
        network_example::HitVolumeSnapshot right_leg = left_leg;
        right_leg.center.x = 4.0f;
        network_example::HitVolumeSnapshot body = left_leg;
        body.center = glm::vec3{0.0f, 14.0f, 0.0f};
        body.half_extents = glm::vec3{6.0f, 3.0f, 3.0f};
        body.hit_zone = 150u;
        network_example::HistoryFrame rig = frame_with(bounds);
        rig.volumes.push_back(left_leg);
        rig.volumes.push_back(right_leg);
        rig.volumes.push_back(body);

        const glm::vec3 north{0.0f, 0.0f, 1.0f};
        network_example::HistoricalHitResult hit;

        // Between the legs, under the body: inside the bounds, on nothing.
        // Before bounds this was a hit on the box.
        require(!network_example::raycast_history_frame(
            rig, glm::vec3{0.0f, 3.0f, -30.0f}, north, 100.0f, 0u, &hit));

        // Through a leg, with include_limbs left false: the bones are this
        // actor's body, not an opt-in.
        require(network_example::raycast_history_frame(
            rig, glm::vec3{-4.0f, 3.0f, -30.0f}, north, 100.0f, 0u, &hit));
        require(hit.net_id == 50u);
        require(hit.volume.is_limb == 1u);
        require(hit.volume.hit_zone == 50u);
        // At the leg's face, not the bounds' face 11 m nearer.
        require(std::fabs(hit.distance - 29.0f) < 0.001f);

        // Through the body: its own multiplier.
        require(network_example::raycast_history_frame(
            rig, glm::vec3{0.0f, 14.0f, -30.0f}, north, 100.0f, 0u, &hit));
        require(hit.volume.hit_zone == 150u);

        // The gate. A bone outside the bounds is never tested, even on a
        // line that crosses it, and even when the caller asks for limbs.
        network_example::HitVolumeSnapshot stray = left_leg;
        stray.center = glm::vec3{40.0f, 5.0f, 0.0f};
        network_example::HistoryFrame gated = frame_with(bounds);
        gated.volumes.push_back(stray);
        require(!network_example::raycast_history_frame(
            gated, glm::vec3{40.0f, 5.0f, -30.0f}, north, 100.0f, 0u, &hit,
            true));
        // The control: the same stray bone with no bounds in the frame is an
        // ordinary limb, and the same ray finds it when limbs are asked for.
        const network_example::HistoryFrame ungated = frame_with(stray);
        require(network_example::raycast_history_frame(
            ungated, glm::vec3{40.0f, 5.0f, -30.0f}, north, 100.0f, 0u, &hit,
            true));

        // A dead or ignored actor's bones are skipped with its bounds.
        network_example::HistoryFrame dead_rig = rig;
        for (network_example::HitVolumeSnapshot& volume : dead_rig.volumes) {
            volume.alive = 0u;
        }
        require(!network_example::raycast_history_frame(
            dead_rig, glm::vec3{-4.0f, 3.0f, -30.0f}, north, 100.0f, 0u, &hit));
        require(!network_example::raycast_history_frame(
            rig, glm::vec3{-4.0f, 3.0f, -30.0f}, north, 100.0f, 50u, &hit));
    }

    // Recording carries Hitbox::bounds_only into the frame.
    {
        network_example::World world;
        const network_example::NetId rigged = world.spawn_entity(
            network_example::EntityType::kActor,
            network_example::ActorType::kAgent,
            0u,
            glm::vec3{0.0f, 0.0f, 0.0f});
        const auto entity = world.find_entity(rigged);
        require(entity.has_value());
        network_example::Hitbox bounds_box{
            glm::vec3{0.0f, 12.0f, 0.0f},
            glm::vec3{12.0f, 14.0f, 12.0f},
            0u};
        bounds_box.bounds_only = true;
        world.registry().emplace_or_replace<network_example::Hitbox>(
            *entity, bounds_box);
        network_example::HistoryBuffer history(4u);
        history.write_frame(world, 3u);
        const network_example::HistoryFrame* recorded = history.find_frame(3u);
        require(recorded != nullptr);
        bool saw_bounds = false;
        for (const network_example::HitVolumeSnapshot& volume : recorded->volumes) {
            if (volume.net_id == rigged) {
                saw_bounds = volume.is_bounds == 1u;
            }
        }
        require(saw_bounds);
        // With no bones recorded there is nothing to hit at all.
        network_example::HistoricalHitResult hit;
        require(!network_example::raycast_history_frame(
            *recorded,
            glm::vec3{0.0f, 12.0f, -30.0f},
            glm::vec3{0.0f, 0.0f, 1.0f},
            100.0f,
            0u,
            &hit));
    }

    // Recording. A placed prop is in the rewound world -- a lag-compensated shot
    // has to be able to land on it -- and a carried one is not: its collider is
    // off while it rides the carrier, and a shot at the carrier must not stop on
    // what they hold.
    {
        network_example::World world;
        const network_example::NetId placed = world.spawn_entity(
            network_example::EntityType::kProp,
            network_example::ActorType::kUnknown,
            0u,
            glm::vec3{0.0f, 0.0f, 0.0f});
        const network_example::NetId carried = world.spawn_entity(
            network_example::EntityType::kProp,
            network_example::ActorType::kUnknown,
            0u,
            glm::vec3{5.0f, 0.0f, 0.0f});
        for (const network_example::NetId net_id : {placed, carried}) {
            const auto entity = world.find_entity(net_id);
            require(entity.has_value());
            world.registry().emplace<network_example::Hitbox>(
                *entity,
                network_example::Hitbox{
                    glm::vec3{0.0f, 1.0f, 0.0f},
                    glm::vec3{1.0f, 1.0f, 1.0f},
                    0u});
            world.registry().emplace<network_example::PropWorldMode>(
                *entity,
                network_example::PropWorldMode{network_example::PropMode::kPlaced});
        }
        world.registry()
            .get<network_example::PropWorldMode>(*world.find_entity(carried))
            .mode = network_example::PropMode::kCarrying;

        network_example::HistoryBuffer history(4u);
        history.write_frame(world, 7u);
        const network_example::HistoryFrame* recorded = history.find_frame(7u);
        require(recorded != nullptr);
        bool saw_placed = false;
        bool saw_carried = false;
        for (const network_example::HitVolumeSnapshot& volume : recorded->volumes) {
            saw_placed = saw_placed || volume.net_id == placed;
            saw_carried = saw_carried || volume.net_id == carried;
        }
        require(saw_placed);
        require(!saw_carried);

        network_example::HistoricalHitResult hit;
        require(network_example::raycast_history_frame(
            *recorded,
            glm::vec3{-10.0f, 1.0f, 0.0f},
            glm::vec3{1.0f, 0.0f, 0.0f},
            100.0f,
            0u,
            &hit));
        require(hit.net_id == placed);
    }

    std::printf("history_buffer_test: PASS\n");
    return 0;
}
