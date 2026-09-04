#include "game_server/src/patrol_navigation.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <DetourStatus.h>

namespace network_example::game_server {
namespace {

// geometry/public/mesh_bake.h owns these. Duplicated rather than depended on so
// that game_server does not pull the Recast bake path in behind Detour; the
// same duplication patrol_nav_bench makes, for the same reason.
constexpr std::size_t kMeshArtifactHeaderSize = 88;
constexpr std::array<std::uint8_t, 8> kRecastMagic = {
    'N', 'K', 'M', 'R', 'C', 'S', 'T', '1'};
constexpr std::uint32_t kLittleEndian = 1;
// A route across a large level crosses a lot of polygons; hitting this is a
// route to reject rather than a limit to work around.
constexpr int kMaxPathPolys = 1024;
constexpr int kMaxStraightPathPoints = 512;

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    }
    return value;
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    }
    return value;
}

float horizontal_distance(const float* from, const float* to) {
    const float delta_x = to[0] - from[0];
    const float delta_z = to[2] - from[2];
    return std::sqrt(delta_x * delta_x + delta_z * delta_z);
}

}  // namespace

PatrolNavigation::PatrolNavigation() = default;

PatrolNavigation::~PatrolNavigation() {
    if (query_ != nullptr) {
        dtFreeNavMeshQuery(query_);
    }
    if (mesh_ != nullptr) {
        dtFreeNavMesh(mesh_);
    }
}

PatrolNavigation::PatrolNavigation(PatrolNavigation&& other) noexcept
    : mesh_(other.mesh_), query_(other.query_) {
    other.mesh_ = nullptr;
    other.query_ = nullptr;
}

PatrolNavigation& PatrolNavigation::operator=(PatrolNavigation&& other) noexcept {
    if (this != &other) {
        std::swap(mesh_, other.mesh_);
        std::swap(query_, other.query_);
    }
    return *this;
}

bool PatrolNavigation::load(
    const std::vector<std::uint8_t>& artifact,
    std::string* error) {
    const auto fail = [error](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (artifact.size() < kMeshArtifactHeaderSize ||
        !std::equal(kRecastMagic.begin(), kRecastMagic.end(), artifact.begin())) {
        return fail("navmesh artifact magic or header is wrong");
    }
    const std::uint32_t payload_endianness = read_u32(artifact, 24);
    const std::uint64_t payload_size = read_u64(artifact, 56);
    if (artifact.size() < kMeshArtifactHeaderSize + payload_size) {
        return fail("navmesh artifact is truncated");
    }

    auto* nav_data = static_cast<unsigned char*>(
        dtAlloc(static_cast<int>(payload_size), DT_ALLOC_PERM));
    if (nav_data == nullptr) {
        return fail("navmesh allocation failed");
    }
    std::memcpy(
        nav_data,
        artifact.data() + kMeshArtifactHeaderSize,
        static_cast<std::size_t>(payload_size));
    if (payload_endianness != kLittleEndian) {
        if (!dtNavMeshHeaderSwapEndian(nav_data, static_cast<int>(payload_size)) ||
            !dtNavMeshDataSwapEndian(nav_data, static_cast<int>(payload_size))) {
            dtFree(nav_data);
            return fail("navmesh endian conversion failed");
        }
    }

    mesh_ = dtAllocNavMesh();
    // DT_TILE_FREE_DATA hands the allocation to the mesh, so nav_data must not
    // be freed here once init has succeeded.
    if (mesh_ == nullptr ||
        dtStatusFailed(mesh_->init(
            nav_data, static_cast<int>(payload_size), DT_TILE_FREE_DATA))) {
        dtFree(nav_data);
        return fail("navmesh init failed");
    }
    query_ = dtAllocNavMeshQuery();
    if (query_ == nullptr || dtStatusFailed(query_->init(mesh_, kMaxPathPolys))) {
        return fail("navmesh query init failed");
    }
    return true;
}

bool PatrolNavigation::valid() const {
    return mesh_ != nullptr && query_ != nullptr;
}

bool PatrolNavigation::snap(
    const KernelVec3& position,
    float half_extent_meters,
    KernelVec3* out_position) const {
    if (!valid() || out_position == nullptr) {
        return false;
    }
    const float center[3] = {position.x, position.y, position.z};
    // Vertically generous: an authored point carries whatever height the area
    // was authored at, which need not be the ground's.
    const float half_extents[3] = {
        half_extent_meters,
        1000.0f,
        half_extent_meters,
    };
    const dtQueryFilter filter;
    dtPolyRef ref = 0;
    float nearest[3] = {0.0f, 0.0f, 0.0f};
    if (dtStatusFailed(
            query_->findNearestPoly(center, half_extents, &filter, &ref, nearest)) ||
        ref == 0) {
        return false;
    }
    *out_position = KernelVec3{nearest[0], nearest[1], nearest[2]};
    return true;
}

bool PatrolNavigation::find_route(
    const PatrolRouteRequest& request,
    std::vector<KernelVec3>* waypoints) const {
    if (!valid() || waypoints == nullptr) {
        return false;
    }
    const float half_extents[3] = {
        request.snap_half_extent_meters,
        1000.0f,
        request.snap_half_extent_meters,
    };
    const dtQueryFilter filter;

    const float start_center[3] = {
        request.start.x, request.start.y, request.start.z};
    const float end_center[3] = {request.end.x, request.end.y, request.end.z};
    dtPolyRef start_ref = 0;
    dtPolyRef end_ref = 0;
    float start[3] = {0.0f, 0.0f, 0.0f};
    float end[3] = {0.0f, 0.0f, 0.0f};
    if (dtStatusFailed(query_->findNearestPoly(
            start_center, half_extents, &filter, &start_ref, start)) ||
        start_ref == 0 ||
        dtStatusFailed(query_->findNearestPoly(
            end_center, half_extents, &filter, &end_ref, end)) ||
        end_ref == 0) {
        return false;
    }

    std::array<dtPolyRef, kMaxPathPolys> path{};
    int path_count = 0;
    const dtStatus status = query_->findPath(
        start_ref, end_ref, start, end, &filter, path.data(), &path_count,
        kMaxPathPolys);
    // A partial result comes back as a valid status with a corridor that stops
    // short, so being connected has to be checked on the end polygon rather
    // than on the status.
    if (!dtStatusSucceed(status) || path_count <= 0 ||
        path[path_count - 1] != end_ref) {
        return false;
    }

    std::array<float, kMaxStraightPathPoints * 3> corners{};
    int corner_count = 0;
    if (dtStatusFailed(query_->findStraightPath(
            start, end, path.data(), path_count, corners.data(), nullptr,
            nullptr, &corner_count, kMaxStraightPathPoints)) ||
        corner_count < 2) {
        return false;
    }

    float path_meters = 0.0f;
    for (int corner = 1; corner < corner_count; ++corner) {
        path_meters +=
            horizontal_distance(&corners[(corner - 1) * 3], &corners[corner * 3]);
    }
    const float line_meters = horizontal_distance(start, end);
    if (request.max_detour_ratio > 0.0f && line_meters > 0.0f &&
        path_meters > line_meters * request.max_detour_ratio) {
        return false;
    }

    // The first corner is where the squad already is; the waypoints are what it
    // has left to walk.
    waypoints->clear();
    waypoints->reserve(static_cast<std::size_t>(corner_count - 1));
    for (int corner = 1; corner < corner_count; ++corner) {
        waypoints->push_back(KernelVec3{
            corners[corner * 3],
            corners[corner * 3 + 1],
            corners[corner * 3 + 2],
        });
    }
    return !waypoints->empty();
}

}  // namespace network_example::game_server
