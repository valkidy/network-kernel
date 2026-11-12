#include "game_server/src/scene_placement.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "game_server/src/scene_table.h"
#include "physics/public/physics_world.h"

namespace network_example::game_server {

namespace {

// How far inside an edge the ground counts toward that edge's wall height.
constexpr float kEdgeBandMeters = 3.0f;
// Below the lowest ground in a segment, so no seam opens under a wall.
constexpr float kWallFootMeters = 1.0f;
// A grounded prop lands on the highest surface at most this far above where
// it was authored: a prop authored on a floor under a bridge stays under it.
constexpr float kGroundSearchAboveMeters = 2.0f;

struct Triangle {
    KernelVec3 a;
    KernelVec3 b;
    KernelVec3 c;
};

// The surface height of `t` straight above or below (x, z), if (x, z) is
// inside it seen from above.
std::optional<float> height_on(const Triangle& t, float x, float z) {
    const float d = (t.b.z - t.c.z) * (t.a.x - t.c.x) + (t.c.x - t.b.x) * (t.a.z - t.c.z);
    if (std::fabs(d) < 1e-9f) {
        return std::nullopt;  // edge-on from above: a wall, not a floor
    }
    const float u = ((t.b.z - t.c.z) * (x - t.c.x) + (t.c.x - t.b.x) * (z - t.c.z)) / d;
    const float v = ((t.c.z - t.a.z) * (x - t.c.x) + (t.a.x - t.c.x) * (z - t.c.z)) / d;
    const float w = 1.0f - u - v;
    constexpr float kSlack = -1e-5f;
    if (u < kSlack || v < kSlack || w < kSlack) {
        return std::nullopt;
    }
    return u * t.a.y + v * t.b.y + w * t.c.y;
}

struct GroundModel {
    const terrain::HeightGrid* heights = nullptr;
    std::vector<Triangle> triangles;
    std::vector<KernelVec3> vertices;
};

bool read_ground(const SceneGround& ground, GroundModel* out, std::string* error) {
    if (ground.heights != nullptr) {
        out->heights = ground.heights;
        return true;
    }
    if (ground.mesh_artifact.empty()) {
        return true;  // no ground known: nothing grounded can be placed
    }
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    if (!physics::static_scene_triangles(ground.mesh_artifact, &positions, &indices, error)) {
        return false;
    }
    out->vertices.reserve(positions.size() / 3u);
    for (std::size_t i = 0; i + 2u < positions.size(); i += 3u) {
        out->vertices.push_back(KernelVec3{positions[i], positions[i + 1u], positions[i + 2u]});
    }
    out->triangles.reserve(indices.size() / 3u);
    for (std::size_t i = 0; i + 2u < indices.size(); i += 3u) {
        out->triangles.push_back(Triangle{
            out->vertices[indices[i]], out->vertices[indices[i + 1u]], out->vertices[indices[i + 2u]]});
    }
    return true;
}

std::optional<float> ground_under(const GroundModel& ground, float x, float y, float z) {
    if (ground.heights != nullptr) {
        const float half_x = static_cast<float>(ground.heights->samples_x - 1) * 0.15f;
        const float half_z = static_cast<float>(ground.heights->samples_z - 1) * 0.15f;
        if (std::fabs(x) > half_x || std::fabs(z) > half_z) {
            return std::nullopt;
        }
        return terrain_ground_height(*ground.heights, x, z);
    }
    std::optional<float> best;
    for (const Triangle& triangle : ground.triangles) {
        const std::optional<float> h = height_on(triangle, x, z);
        if (h && *h <= y + kGroundSearchAboveMeters && (!best || *h > *best)) {
            best = h;
        }
    }
    return best;
}

}  // namespace

std::vector<KernelPlayerBarrierBox> map_edge_barriers(
    const MapEdgeBarrierConfig& rule,
    float min_x,
    float min_z,
    float max_x,
    float max_z,
    const std::vector<KernelVec3>& ground_points) {
    std::vector<KernelPlayerBarrierBox> walls;
    if (!rule.enabled || !(max_x > min_x) || !(max_z > min_z)) {
        return walls;
    }
    const float t = rule.thickness_meters;
    const float band = std::max(kEdgeBandMeters, rule.inset_meters + t + 1.0f);

    // An edge: the line `fixed` = value on the perpendicular axis, running
    // [from, to] along the other; `inward` is +1 or -1 on the perpendicular.
    struct Edge {
        bool along_x;
        float fixed;
        float inward;
        float from;
        float to;
    };
    const Edge edges[4] = {
        {true, min_z, +1.0f, min_x, max_x},
        {true, max_z, -1.0f, min_x, max_x},
        {false, min_x, +1.0f, min_z, max_z},
        {false, max_x, -1.0f, min_z, max_z},
    };

    float segment = rule.segment_meters;
    const float perimeter = 2.0f * ((max_x - min_x) + (max_z - min_z));
    const float budget = static_cast<float>(KERNEL_MAX_PLAYER_BARRIERS_PER_SLOT) - 8.0f;
    segment = std::max(segment, perimeter / budget);

    for (const Edge& edge : edges) {
        const float length = edge.to - edge.from;
        const int count = std::max(1, static_cast<int>(std::ceil(length / segment - 1e-4f)));
        const float step = length / static_cast<float>(count);
        for (int s = 0; s < count; ++s) {
            const float a0 = edge.from + step * static_cast<float>(s);
            const float a1 = a0 + step;
            float low = std::numeric_limits<float>::infinity();
            float high = -std::numeric_limits<float>::infinity();
            for (const KernelVec3& p : ground_points) {
                const float along = edge.along_x ? p.x : p.z;
                const float across = edge.along_x ? p.z : p.x;
                const float inside = (across - edge.fixed) * edge.inward;
                if (along < a0 || along > a1 || inside < -0.01f || inside > band) {
                    continue;
                }
                low = std::min(low, p.y);
                high = std::max(high, p.y);
            }
            if (!std::isfinite(low)) {
                continue;  // no ground at this stretch of edge: nothing to walk off
            }
            const float bottom = low - kWallFootMeters;
            const float top = high + rule.height_meters;
            const float across_center = edge.fixed + edge.inward * (rule.inset_meters + t * 0.5f);
            const float along_center = (a0 + a1) * 0.5f;
            // Each run reaches past its ends by the thickness, so corners and
            // the seams between segments close.
            const float half_along = step * 0.5f + t;
            KernelPlayerBarrierBox box{};
            box.center = edge.along_x
                ? KernelVec3{along_center, (bottom + top) * 0.5f, across_center}
                : KernelVec3{across_center, (bottom + top) * 0.5f, along_center};
            box.half_extents = edge.along_x
                ? KernelVec3{half_along, (top - bottom) * 0.5f, t * 0.5f}
                : KernelVec3{t * 0.5f, (top - bottom) * 0.5f, half_along};
            box.yaw_radians = 0.0f;
            walls.push_back(box);
        }
    }
    return walls;
}

bool finish_scene_placement(
    GameServerGameplayConfig* config,
    const SceneGround& ground,
    std::string* error) {
    GroundModel model;
    if (!read_ground(ground, &model, error)) {
        *error = "scene ground does not read: " + *error;
        return false;
    }
    for (ScenePropConfig& prop : config->scene_props) {
        if (!prop.grounded) {
            continue;
        }
        const std::optional<float> y =
            ground_under(model, prop.position.x, prop.position.y, prop.position.z);
        if (!y) {
            *error = "grounded scene prop (template " + std::to_string(prop.entity_template_id) +
                ") has no ground under (" + std::to_string(prop.position.x) + ", " +
                std::to_string(prop.position.z) + ")";
            return false;
        }
        prop.position.y = *y;
    }

    config->resolved_player_barriers = config->player_barriers;
    if (config->map_edge_barrier.enabled) {
        std::vector<KernelVec3> points;
        float min_x = 0.0f, min_z = 0.0f, max_x = 0.0f, max_z = 0.0f;
        if (model.heights != nullptr) {
            const terrain::HeightGrid& h = *model.heights;
            max_x = static_cast<float>(h.samples_x - 1) * 0.15f;
            max_z = static_cast<float>(h.samples_z - 1) * 0.15f;
            min_x = -max_x;
            min_z = -max_z;
            const int band = static_cast<int>(std::ceil(kEdgeBandMeters / 0.3f)) + 8;
            for (std::int32_t j = 0; j < h.samples_z; ++j) {
                for (std::int32_t i = 0; i < h.samples_x; ++i) {
                    const bool near_edge = i < band || j < band ||
                        i >= h.samples_x - band || j >= h.samples_z - band;
                    if (!near_edge) {
                        continue;
                    }
                    points.push_back(KernelVec3{
                        static_cast<float>(i) * 0.3f + min_x,
                        static_cast<float>(h.at(i, j)) / 100.0f,
                        static_cast<float>(j) * 0.3f + min_z});
                }
            }
        } else if (!model.vertices.empty()) {
            min_x = max_x = model.vertices.front().x;
            min_z = max_z = model.vertices.front().z;
            for (const KernelVec3& v : model.vertices) {
                min_x = std::min(min_x, v.x);
                max_x = std::max(max_x, v.x);
                min_z = std::min(min_z, v.z);
                max_z = std::max(max_z, v.z);
            }
            points = model.vertices;
        } else {
            *error = "map_edge_barrier needs the scene's ground, and none was given";
            return false;
        }
        const std::vector<KernelPlayerBarrierBox> walls =
            map_edge_barriers(config->map_edge_barrier, min_x, min_z, max_x, max_z, points);
        config->resolved_player_barriers.insert(
            config->resolved_player_barriers.end(), walls.begin(), walls.end());
    }
    if (config->resolved_player_barriers.size() > KERNEL_MAX_PLAYER_BARRIERS_PER_SLOT) {
        *error = "scene has " + std::to_string(config->resolved_player_barriers.size()) +
            " player barriers; a slot takes at most " +
            std::to_string(KERNEL_MAX_PLAYER_BARRIERS_PER_SLOT);
        return false;
    }
    return true;
}

}  // namespace network_example::game_server
