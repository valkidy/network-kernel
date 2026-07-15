#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "geometry/public/canonical_triangle_mesh.h"

namespace network_example {

inline constexpr std::uint32_t kMeshArtifactSchemaVersion = 1;
inline constexpr std::size_t kMeshArtifactHeaderSize = 88;
inline constexpr std::size_t kMeshArtifactBackendVersionOffset = 16;

struct MeshBakeConfig {
    std::uint32_t jolt_max_triangles_per_leaf = 8;
    bool jolt_favor_runtime_performance = true;
    float recast_cell_size = 0.3f;
    float recast_cell_height = 0.2f;
    float recast_agent_height = 2.0f;
    float recast_agent_radius = 0.6f;
    float recast_agent_max_climb = 0.9f;
    float recast_walkable_slope_degrees = 45.0f;
    float recast_region_min_size = 0.0f;
    float recast_region_merge_size = 0.0f;
    float recast_edge_max_length = 12.0f;
    float recast_edge_max_error = 1.3f;
    std::uint32_t recast_max_vertices_per_polygon = 6;
    float recast_detail_sample_distance_factor = 6.0f;
    float recast_detail_sample_max_error_factor = 1.0f;
};

struct MeshBakeConfigResult {
    MeshBakeConfig config;
    std::string error;

    explicit operator bool() const { return error.empty(); }
};

struct MeshBakeArtifacts {
    std::vector<std::uint8_t> jolt;
    std::vector<std::uint8_t> recast;
};

struct MeshBakeResult {
    MeshBakeArtifacts artifacts;
    std::string error;

    explicit operator bool() const { return error.empty(); }
};

MeshBakeConfig default_mesh_bake_config();
MeshBakeConfigResult load_mesh_bake_config_file(const std::string& path);

std::uint64_t fnv1a_file_hash(const std::string& path, std::string* error);

MeshBakeResult bake_mesh_artifacts(
    const CanonicalTriangleMesh& mesh,
    const MeshBakeConfig& config,
    std::uint64_t source_hash,
    std::uint64_t config_hash);

bool validate_jolt_mesh_artifact(
    const std::vector<std::uint8_t>& artifact,
    const CanonicalTriangleMesh& source_mesh,
    std::uint64_t expected_source_hash,
    std::uint64_t expected_config_hash,
    std::string* error);

bool validate_recast_mesh_artifact(
    const std::vector<std::uint8_t>& artifact,
    std::uint64_t expected_source_hash,
    std::uint64_t expected_config_hash,
    std::string* error);

}  // namespace network_example
