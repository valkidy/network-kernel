#include "game_server/patrol_navigation.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

std::filesystem::path runfiles_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr);
    require(test_workspace != nullptr);
    return std::filesystem::path(test_srcdir) / test_workspace;
}

std::vector<std::uint8_t> read_binary_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

float horizontal_distance(const KernelVec3& from, const KernelVec3& to) {
    const float delta_x = to.x - from.x;
    const float delta_z = to.z - from.z;
    return std::sqrt(delta_x * delta_x + delta_z * delta_z);
}

float route_length(const KernelVec3& start, const std::vector<KernelVec3>& waypoints) {
    float total = 0.0f;
    KernelVec3 previous = start;
    for (const KernelVec3& waypoint : waypoints) {
        total += horizontal_distance(previous, waypoint);
        previous = waypoint;
    }
    return total;
}

}  // namespace

// obstructed_field is the terrain //game_server:patrol_nav_bench measured: flat
// ground with pits, four walls in a zigzag, and one sealed courtyard. Every
// claim below is about a feature of it that a straight chord cannot see.
int main() {
    using network_example::game_server::PatrolNavigation;
    using network_example::game_server::PatrolRouteRequest;

    const std::vector<std::uint8_t> artifact = read_binary_file(
        (runfiles_root() / "game_server" / "test_mesh_assets" / "generated" /
         "recast" / "obstructed_field.navmesh")
            .string());

    PatrolNavigation navigation;
    std::string error;
    require(navigation.load(artifact, &error));
    require(error.empty());
    require(navigation.valid());

    // Rubbish is rejected rather than crashed on.
    PatrolNavigation broken;
    std::vector<std::uint8_t> truncated(artifact.begin(), artifact.begin() + 40);
    require(!broken.load(truncated, &error));
    require(!error.empty());
    require(!broken.valid());

    // Open ground snaps to itself. A point in the middle of a pit does not,
    // within a radius that would not reach the pit's edge.
    KernelVec3 snapped{0.0f, 0.0f, 0.0f};
    require(navigation.snap(KernelVec3{20.0f, 0.0f, 5.0f}, 1.0f, &snapped));
    require(horizontal_distance(snapped, KernelVec3{20.0f, 0.0f, 5.0f}) < 1.0f);
    // The central pit is x[-7.5, 7.5] z[-7.5, 7.5]; its middle is 7 m from the
    // nearest walkable ground.
    require(!navigation.snap(KernelVec3{0.0f, 0.0f, 0.0f}, 1.0f, &snapped));

    // A route across open ground, and it is a route rather than a pair of ends:
    // the corners are what the squad walks.
    PatrolRouteRequest open;
    open.start = KernelVec3{20.0f, 0.0f, 5.0f};
    open.end = KernelVec3{40.0f, 0.0f, 5.0f};
    std::vector<KernelVec3> waypoints;
    require(navigation.find_route(open, &waypoints));
    require(!waypoints.empty());
    require(horizontal_distance(waypoints.back(), open.end) < 1.5f);

    // The sealed courtyard is walkable inside and has no way in, so no route
    // reaches it. A find_route that reported partial paths as successes would
    // return one anyway, which is why the corridor's end polygon is checked.
    PatrolRouteRequest sealed;
    sealed.start = KernelVec3{20.0f, 0.0f, 5.0f};
    sealed.end = KernelVec3{-37.5f, 0.0f, 37.5f};
    require(!navigation.find_route(sealed, &waypoints));

    // Two points either side of a wall: close together, and a long way apart to
    // walk. This is the shape the bench found at 18.10x, and the shape a squad
    // must not be given.
    PatrolRouteRequest across_wall;
    across_wall.start = KernelVec3{-20.0f, 0.0f, -25.0f};
    across_wall.end = KernelVec3{-20.0f, 0.0f, -10.0f};
    require(navigation.snap(across_wall.start, 1.0f, &across_wall.start));
    require(navigation.snap(across_wall.end, 1.0f, &across_wall.end));

    across_wall.max_detour_ratio = 0.0f;
    require(navigation.find_route(across_wall, &waypoints));
    const float line = horizontal_distance(across_wall.start, across_wall.end);
    const float walked = route_length(across_wall.start, waypoints);
    // The wall is really in the way, or the rejection below would be measuring
    // nothing.
    require(walked > line * 3.0f);

    across_wall.max_detour_ratio = 2.0f;
    require(!navigation.find_route(across_wall, &waypoints));
    across_wall.max_detour_ratio = walked / line + 1.0f;
    require(navigation.find_route(across_wall, &waypoints));

    return 0;
}
