#ifndef GAME_SERVER_PATROL_NAVIGATION_H_
#define GAME_SERVER_PATROL_NAVIGATION_H_

#include <cstdint>
#include <string>
#include <vector>

#include "kernel/public/kernel_types.h"

class dtNavMesh;
class dtNavMeshQuery;

namespace network_example::game_server {

struct PatrolRouteRequest {
    KernelVec3 start{0.0f, 0.0f, 0.0f};
    KernelVec3 end{0.0f, 0.0f, 0.0f};
    // How far off the navmesh a requested point may be and still be pulled onto
    // it. Roughly an agent radius: close enough to stand, rather than close
    // enough to be somewhere else.
    float snap_half_extent_meters = 1.0f;
    // Reject a route that walks much further than the straight line between its
    // ends. //game_server:patrol_nav_bench measured a worst case of 18.10 on a
    // built level -- two points 20 m apart needing a 270 m walk -- which is a
    // squad that would spend its whole life going round one wall. Zero accepts
    // any route that connects at all.
    float max_detour_ratio = 0.0f;
};

// The navmesh, at run time.
//
// The asset has been shipping all along: mesh_asset_bakes emits a .navmesh
// beside every .joltmesh and the catalog bundle already carries it to clients.
// What was missing was anything that kept the loaded result -- mesh_bake.cc
// builds exactly this pair to validate a bake and then throws it away.
//
// It lives in game_server, not the kernel, and takes nothing from the kernel to
// work. Navigation is what the AI decides with; the kernel's movement is driven
// by the character controller and does not know a navmesh exists.
class PatrolNavigation {
public:
    PatrolNavigation();
    ~PatrolNavigation();
    PatrolNavigation(PatrolNavigation&&) noexcept;
    PatrolNavigation& operator=(PatrolNavigation&&) noexcept;
    PatrolNavigation(const PatrolNavigation&) = delete;
    PatrolNavigation& operator=(const PatrolNavigation&) = delete;

    // `artifact` is the whole baked file, header included.
    bool load(const std::vector<std::uint8_t>& artifact, std::string* error);
    bool valid() const;

    // Pulls a point onto walkable ground. False when there is none within
    // `half_extent_meters`, which on a real level is most of what an authored
    // rectangle covers -- the bench measured 89.7% of a walled terrain already
    // walkable and 94.9% once snapped.
    bool snap(
        const KernelVec3& position,
        float half_extent_meters,
        KernelVec3* out_position) const;

    // The corners of a walkable route, start excluded, so the result is the
    // waypoint list a squad walks. False when either end is off the navmesh,
    // when they are not connected, or when the route detours further than the
    // request allows.
    bool find_route(
        const PatrolRouteRequest& request,
        std::vector<KernelVec3>* waypoints) const;

private:
    dtNavMesh* mesh_ = nullptr;
    dtNavMeshQuery* query_ = nullptr;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_PATROL_NAVIGATION_H_
