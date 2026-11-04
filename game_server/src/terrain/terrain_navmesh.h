// Server-side navmesh for a composed heightfield and the checks that decide
// whether a generated map is accepted (implementation plan P2.2 / P2.3).
//
// Bake: Recast, one 48 m tile at a time (each tile sees a 5-cell border of its
// neighbours' ground, so seams are exact), tiles in parallel, assembled into a
// multi-tile dtNavMesh in tile order. The input is the heightfield's own
// triangles, split along the same diagonal as Jolt's HeightFieldShape, in
// world coordinates (map centre at the origin). Parameters are mesh_bake.yaml's.
// The voxel grid is offset half a cell from the samples, so a sample sits in the
// middle of a voxel column rather than on its wall: otherwise a thin feature's
// connectivity depended on float noise, i.e. on where on the map a stamp landed.
//
// Checks (Compose Test Map's BakeAndCheck rules):
//   islands  connected components of the navmesh (polygons joined by links);
//   reach    a 1 m probe grid snapped to the navmesh (within 1.5 m, and at
//            most half a cell off horizontally); reached = on the biggest
//            component;
//   seams    every 2 m along every stamp edge, a pair 1.5 m inside and outside;
//            a pair with navmesh on both sides must connect (else a break) and
//            should path within 7 m (else a detour: reported, not fatal -- a
//            wall inside a narrow rim, which StampExporter already warns of). A pair
//            whose inside point is not within climb of the stamp's border height
//            sits on a feature at the border (a wall top), not on a crossing,
//            and is counted with the no-mesh pairs.
// The server runs both; the client never sees a navmesh.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game_server/src/terrain/terrain_compose.h"

class dtNavMesh;

namespace network_example::game_server::terrain {

struct NavBakeParams {
    float cell_size = 0.3f;
    float cell_height = 0.2f;
    float agent_height = 2.0f;
    float agent_radius = 0.6f;
    float agent_max_climb = 0.9f;
    float walkable_slope_degrees = 45.0f;
    float region_min_size = 0.0f;
    float region_merge_size = 0.0f;
    float edge_max_length = 12.0f;
    float edge_max_error = 1.3f;
    int max_vertices_per_polygon = 6;
    float detail_sample_distance_factor = 6.0f;
    float detail_sample_max_error_factor = 1.0f;
    std::int32_t tile_cells = 160;      // 48 m
    std::uint32_t threads = 0;          // 0 = hardware concurrency
};

struct NavBakeStats {
    std::uint32_t tiles = 0;
    std::uint32_t polys = 0;
    double bake_ms = 0;
};

// Returns a dtNavMesh the caller owns (dtFreeNavMesh), or nullptr.
dtNavMesh* bake_terrain_navmesh(const HeightGrid& grid, const NavBakeParams& params,
    NavBakeStats* stats, std::string* error);

struct NavCheck {
    std::uint32_t islands = 0;
    std::vector<double> island_areas_m2;   // biggest first
    std::uint32_t probes_on_mesh = 0;
    std::uint32_t probes_reached = 0;
    std::uint32_t unreachable_clusters = 0;
    double unreachable_area_m2 = 0;
    std::uint32_t seams_tested = 0;
    std::uint32_t seams_failed = 0;       // no path: the seam is cut
    std::uint32_t seams_detoured = 0;     // a path, but longer than 7 m
    std::uint32_t seams_no_mesh = 0;      // incl. pairs on a feature at the border
    double check_ms = 0;
    // Map-metre probe state for pictures: 0 off the navmesh, 1 reached, 2 not.
    std::int32_t probe_nx = 0;
    std::vector<std::uint8_t> probe_state;
    std::vector<float> seam_failures_xz;   // pairs of map metres

    double reach_fraction() const { return probes_on_mesh == 0 ? 0.0 : static_cast<double>(probes_reached) / probes_on_mesh; }
};

bool check_terrain_navmesh(const dtNavMesh& nav, const HeightGrid& grid, const TerrainRecipe& recipe,
    const TerrainLibrary& library, NavCheck* out, std::string* error);

}  // namespace network_example::game_server::terrain
