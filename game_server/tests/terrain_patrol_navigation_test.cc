// The generator's runtime navmesh drives PatrolNavigation (P2.2): adopt it,
// then snap and route across the map.

#include <DetourNavMesh.h>

#include <cstdio>

#include "game_server/src/patrol_navigation.h"
#include "game_server/src/terrain/terrain_generate.h"
#include "game_server/tests/terrain_test_data.h"

namespace terrain = network_example::game_server::terrain;

int main() {
    const terrain::TerrainLibrary lib = terrain_test::load_library();
    terrain::GenerateRequest req;
    req.base = terrain_test::parity_recipe(lib);
    req.base.placements.clear();
    req.seed = 8;                    // accepted on the first layout (terrain_generate_test)
    req.accept.min_placed = 11;
    terrain::GenerateResult res;
    std::string error;
    require(terrain::generate_terrain(req, lib, &res, &error));
    network_example::game_server::PatrolNavigation nav;
    require(nav.adopt(res.navmesh, &error));
    require(nav.valid());
    // Map centre is the world origin; corners 100 m out on either side.
    network_example::game_server::PatrolRouteRequest route;
    route.start = KernelVec3{-95.0f, 30.0f, -95.0f};
    route.end = KernelVec3{95.0f, 30.0f, 95.0f};
    route.snap_half_extent_meters = 20.0f;
    std::vector<KernelVec3> waypoints;
    require(nav.find_route(route, &waypoints));
    std::printf("route corner to corner: %zu waypoints, ends at (%.1f, %.1f, %.1f)\n", waypoints.size(),
        waypoints.back().x, waypoints.back().y, waypoints.back().z);
    require(!waypoints.empty() && waypoints.back().x > 80.0f && waypoints.back().z > 80.0f);
    require(!nav.adopt(nullptr, &error));
    std::printf("terrain_patrol_navigation_test passed\n");
    return 0;
}
