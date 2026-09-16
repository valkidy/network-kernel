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
