// What the rewound world is shaped like.
//
// Lag compensation tests a shot against HistoryFrame volumes rather than
// against the physics world, so the geometry here has to agree with the
// geometry there. HitVolumeSnapshot has carried a rotation from the start; this
// pins that it is actually used, because testing the world-axis bounds instead
// is close enough for an upright body box and badly wrong for anything long
// lying at an angle -- which is what a rig's leg is.

#include "sync/public/history_buffer.h"

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

    std::printf("history_buffer_test: PASS\n");
    return 0;
}
