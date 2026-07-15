#include "geometry/public/mesh_bake.h"

#include <Jolt/Jolt.h>

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <Recast.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace network_example {
namespace {

constexpr std::array<std::uint8_t, 8> kJoltMagic = {
    'N', 'K', 'M', 'J', 'O', 'L', 'T', '1'};
constexpr std::array<std::uint8_t, 8> kRecastMagic = {
    'N', 'K', 'M', 'R', 'C', 'S', 'T', '1'};
constexpr std::uint32_t kLittleEndian = 1;
constexpr std::uint32_t kBigEndian = 2;
constexpr JPH::ObjectLayer kStaticLayer = 0;
constexpr JPH::BroadPhaseLayer kStaticBroadPhaseLayer(0);
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t jolt_version_id() {
    using JPH::uint64;
    return JPH_VERSION_ID;
}

struct ArtifactHeader {
    std::uint64_t backend_version = 0;
    std::uint32_t payload_endianness = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t triangle_count = 0;
    std::uint64_t source_hash = 0;
    std::uint64_t config_hash = 0;
    std::uint64_t payload_size = 0;
    CanonicalMeshBounds bounds;
};

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    std::uint32_t GetNumBroadPhaseLayers() const override { return 1; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(
        JPH::ObjectLayer /*layer*/) const override {
        return kStaticBroadPhaseLayer;
    }
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(
        JPH::ObjectLayer /*layer1*/,
        JPH::BroadPhaseLayer /*layer2*/) const override {
        return true;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(
        JPH::ObjectLayer /*layer1*/,
        JPH::ObjectLayer /*layer2*/) const override {
        return true;
    }
};

class JoltRuntime final {
public:
    JoltRuntime() {
        JPH::RegisterDefaultAllocator();
        if (JPH::Factory::sInstance != nullptr || !JPH::VerifyJoltVersionID()) {
            return;
        }
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        initialized_ = true;
    }

    ~JoltRuntime() {
        if (!initialized_) {
            return;
        }
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    bool initialized() const { return initialized_; }

private:
    bool initialized_ = false;
};

struct RecastArtifacts {
    rcHeightfield* heightfield = nullptr;
    rcCompactHeightfield* compact_heightfield = nullptr;
    rcContourSet* contour_set = nullptr;
    rcPolyMesh* poly_mesh = nullptr;
    rcPolyMeshDetail* detail_mesh = nullptr;

    ~RecastArtifacts() {
        rcFreePolyMeshDetail(detail_mesh);
        rcFreePolyMesh(poly_mesh);
        rcFreeContourSet(contour_set);
        rcFreeCompactHeightfield(compact_heightfield);
        rcFreeHeightField(heightfield);
    }
};

std::uint32_t native_endianness() {
    if constexpr (std::endian::native == std::endian::little) {
        return kLittleEndian;
    }
    return kBigEndian;
}

void append_u32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes->push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_u64(std::vector<std::uint8_t>* bytes, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        bytes->push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void append_f32(std::vector<std::uint8_t>* bytes, float value) {
    append_u32(bytes, std::bit_cast<std::uint32_t>(value));
}

bool read_u32(
    const std::vector<std::uint8_t>& bytes,
    std::size_t* offset,
    std::uint32_t* value) {
    if (*offset + 4 > bytes.size()) {
        return false;
    }
    *value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        *value |= static_cast<std::uint32_t>(bytes[(*offset)++]) << shift;
    }
    return true;
}

bool read_u64(
    const std::vector<std::uint8_t>& bytes,
    std::size_t* offset,
    std::uint64_t* value) {
    if (*offset + 8 > bytes.size()) {
        return false;
    }
    *value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        *value |= static_cast<std::uint64_t>(bytes[(*offset)++]) << shift;
    }
    return true;
}

bool read_f32(
    const std::vector<std::uint8_t>& bytes,
    std::size_t* offset,
    float* value) {
    std::uint32_t bits = 0;
    if (!read_u32(bytes, offset, &bits)) {
        return false;
    }
    *value = std::bit_cast<float>(bits);
    return true;
}

std::vector<std::uint8_t> wrap_artifact(
    const std::array<std::uint8_t, 8>& magic,
    std::uint64_t backend_version,
    const CanonicalTriangleMesh& mesh,
    std::uint64_t source_hash,
    std::uint64_t config_hash,
    const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> artifact;
    artifact.reserve(kMeshArtifactHeaderSize + payload.size());
    artifact.insert(artifact.end(), magic.begin(), magic.end());
    append_u32(&artifact, kMeshArtifactSchemaVersion);
    append_u32(&artifact, static_cast<std::uint32_t>(kMeshArtifactHeaderSize));
    append_u64(&artifact, backend_version);
    append_u32(&artifact, native_endianness());
    append_u32(&artifact, static_cast<std::uint32_t>(mesh.positions.size()));
    append_u32(
        &artifact,
        static_cast<std::uint32_t>(mesh.triangle_indices.size() / 3));
    append_u32(&artifact, 0);
    append_u64(&artifact, source_hash);
    append_u64(&artifact, config_hash);
    append_u64(&artifact, payload.size());
    for (float value : mesh.bounds.min) {
        append_f32(&artifact, value);
    }
    for (float value : mesh.bounds.max) {
        append_f32(&artifact, value);
    }
    artifact.insert(artifact.end(), payload.begin(), payload.end());
    return artifact;
}

bool parse_artifact(
    const std::vector<std::uint8_t>& artifact,
    const std::array<std::uint8_t, 8>& expected_magic,
    std::uint64_t expected_backend_version,
    std::uint64_t expected_source_hash,
    std::uint64_t expected_config_hash,
    ArtifactHeader* header,
    std::string* error) {
    if (artifact.size() < kMeshArtifactHeaderSize ||
        !std::equal(expected_magic.begin(), expected_magic.end(), artifact.begin())) {
        *error = "invalid mesh artifact magic or truncated header";
        return false;
    }
    std::size_t offset = expected_magic.size();
    std::uint32_t schema_version = 0;
    std::uint32_t header_size = 0;
    std::uint32_t reserved = 0;
    if (!read_u32(artifact, &offset, &schema_version) ||
        !read_u32(artifact, &offset, &header_size) ||
        !read_u64(artifact, &offset, &header->backend_version) ||
        !read_u32(artifact, &offset, &header->payload_endianness) ||
        !read_u32(artifact, &offset, &header->vertex_count) ||
        !read_u32(artifact, &offset, &header->triangle_count) ||
        !read_u32(artifact, &offset, &reserved) ||
        !read_u64(artifact, &offset, &header->source_hash) ||
        !read_u64(artifact, &offset, &header->config_hash) ||
        !read_u64(artifact, &offset, &header->payload_size)) {
        *error = "truncated mesh artifact header";
        return false;
    }
    for (float& value : header->bounds.min) {
        if (!read_f32(artifact, &offset, &value)) {
            *error = "truncated mesh artifact bounds";
            return false;
        }
    }
    for (float& value : header->bounds.max) {
        if (!read_f32(artifact, &offset, &value)) {
            *error = "truncated mesh artifact bounds";
            return false;
        }
    }
    if (schema_version != kMeshArtifactSchemaVersion ||
        header_size != kMeshArtifactHeaderSize || reserved != 0) {
        *error = "unsupported mesh artifact schema";
        return false;
    }
    if (header->backend_version != expected_backend_version) {
        *error = "mesh artifact backend version mismatch";
        return false;
    }
    if (header->payload_endianness != kLittleEndian &&
        header->payload_endianness != kBigEndian) {
        *error = "invalid mesh artifact payload endianness";
        return false;
    }
    if (header->source_hash != expected_source_hash ||
        header->config_hash != expected_config_hash) {
        *error = "mesh artifact input hash mismatch";
        return false;
    }
    if (header->payload_size != artifact.size() - kMeshArtifactHeaderSize) {
        *error = "mesh artifact payload size mismatch";
        return false;
    }
    return true;
}

bool validate_keys(
    const YAML::Node& node,
    const std::unordered_set<std::string>& allowed,
    const std::string& path,
    std::string* error) {
    if (!node || !node.IsMap()) {
        *error = path + " must be a map";
        return false;
    }
    for (const auto& entry : node) {
        const std::string key = entry.first.as<std::string>();
        if (!allowed.contains(key)) {
            *error = "unknown mesh bake config field: " + path + "." + key;
            return false;
        }
    }
    for (const std::string& key : allowed) {
        if (!node[key]) {
            *error = "missing mesh bake config field: " + path + "." + key;
            return false;
        }
    }
    return true;
}

bool validate_config(const MeshBakeConfig& config, std::string* error) {
    if (config.jolt_max_triangles_per_leaf < 1 ||
        config.jolt_max_triangles_per_leaf > 8) {
        *error = "jolt.max_triangles_per_leaf must be between 1 and 8";
        return false;
    }
    if (!(config.recast_cell_size > 0.0f) ||
        !(config.recast_cell_height > 0.0f) ||
        !(config.recast_agent_height > 0.0f) ||
        config.recast_agent_radius < 0.0f ||
        config.recast_agent_max_climb < 0.0f ||
        config.recast_walkable_slope_degrees < 0.0f ||
        config.recast_walkable_slope_degrees >= 90.0f ||
        config.recast_region_min_size < 0.0f ||
        config.recast_region_merge_size < 0.0f ||
        config.recast_edge_max_length < 0.0f ||
        config.recast_edge_max_error < 0.0f ||
        config.recast_max_vertices_per_polygon < 3 ||
        config.recast_max_vertices_per_polygon > DT_VERTS_PER_POLYGON ||
        config.recast_detail_sample_distance_factor < 0.0f ||
        config.recast_detail_sample_max_error_factor < 0.0f) {
        *error = "mesh bake config contains an out-of-range Recast value";
        return false;
    }
    return true;
}

bool flatten_mesh(
    const CanonicalTriangleMesh& mesh,
    std::vector<float>* positions,
    std::vector<int>* indices,
    std::string* error) {
    if (mesh.positions.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        mesh.triangle_indices.size() / 3 >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        *error = "mesh exceeds Recast count limits";
        return false;
    }
    positions->reserve(mesh.positions.size() * 3);
    for (const auto& position : mesh.positions) {
        positions->insert(positions->end(), position.begin(), position.end());
    }
    indices->reserve(mesh.triangle_indices.size());
    for (std::uint32_t index : mesh.triangle_indices) {
        if (index > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            *error = "mesh index exceeds Recast limits";
            return false;
        }
        indices->push_back(static_cast<int>(index));
    }
    return true;
}

bool bake_jolt_payload(
    const CanonicalTriangleMesh& mesh,
    const MeshBakeConfig& config,
    std::vector<std::uint8_t>* payload,
    std::string* error) {
    JoltRuntime runtime;
    if (!runtime.initialized()) {
        *error = "failed to initialize Jolt runtime";
        return false;
    }
    JPH::VertexList vertices;
    vertices.reserve(mesh.positions.size());
    for (const auto& position : mesh.positions) {
        vertices.emplace_back(position[0], position[1], position[2]);
    }
    JPH::IndexedTriangleList triangles;
    triangles.reserve(mesh.triangle_indices.size() / 3);
    for (std::size_t i = 0; i < mesh.triangle_indices.size(); i += 3) {
        triangles.emplace_back(
            mesh.triangle_indices[i],
            mesh.triangle_indices[i + 1],
            mesh.triangle_indices[i + 2]);
    }
    JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
    settings.mMaxTrianglesPerLeaf = config.jolt_max_triangles_per_leaf;
    settings.mBuildQuality = config.jolt_favor_runtime_performance
        ? JPH::MeshShapeSettings::EBuildQuality::FavorRuntimePerformance
        : JPH::MeshShapeSettings::EBuildQuality::FavorBuildSpeed;
    JPH::ShapeSettings::ShapeResult shape_result = settings.Create();
    if (shape_result.HasError()) {
        *error = "Jolt MeshShape creation failed: " + shape_result.GetError();
        return false;
    }
    std::ostringstream output(std::ios::binary | std::ios::out);
    JPH::StreamOutWrapper stream(output);
    JPH::Shape::ShapeToIDMap shape_map;
    JPH::Shape::MaterialToIDMap material_map;
    shape_result.Get()->SaveWithChildren(stream, shape_map, material_map);
    if (stream.IsFailed()) {
        *error = "Jolt MeshShape serialization failed";
        return false;
    }
    const std::string bytes = output.str();
    payload->assign(bytes.begin(), bytes.end());
    return true;
}

bool bake_recast_payload(
    const CanonicalTriangleMesh& mesh,
    const MeshBakeConfig& bake_config,
    std::vector<std::uint8_t>* payload,
    std::string* error) {
    std::vector<float> positions;
    std::vector<int> indices;
    if (!flatten_mesh(mesh, &positions, &indices, error)) {
        return false;
    }

    rcContext context;
    rcConfig config{};
    config.cs = bake_config.recast_cell_size;
    config.ch = bake_config.recast_cell_height;
    config.walkableSlopeAngle = bake_config.recast_walkable_slope_degrees;
    config.walkableHeight = static_cast<int>(
        std::ceil(bake_config.recast_agent_height / config.ch));
    config.walkableClimb = static_cast<int>(
        std::floor(bake_config.recast_agent_max_climb / config.ch));
    config.walkableRadius = static_cast<int>(
        std::ceil(bake_config.recast_agent_radius / config.cs));
    config.maxEdgeLen = static_cast<int>(
        bake_config.recast_edge_max_length / config.cs);
    config.maxSimplificationError = bake_config.recast_edge_max_error;
    config.minRegionArea = static_cast<int>(
        std::pow(bake_config.recast_region_min_size, 2.0f));
    config.mergeRegionArea = static_cast<int>(
        std::pow(bake_config.recast_region_merge_size, 2.0f));
    config.maxVertsPerPoly = bake_config.recast_max_vertices_per_polygon;
    config.detailSampleDist = config.cs *
        bake_config.recast_detail_sample_distance_factor;
    config.detailSampleMaxError = config.ch *
        bake_config.recast_detail_sample_max_error_factor;

    const int vertex_count = static_cast<int>(mesh.positions.size());
    const int triangle_count = static_cast<int>(mesh.triangle_indices.size() / 3);
    rcCalcBounds(positions.data(), vertex_count, config.bmin, config.bmax);
    rcCalcGridSize(
        config.bmin, config.bmax, config.cs, &config.width, &config.height);

    RecastArtifacts artifacts;
    artifacts.heightfield = rcAllocHeightfield();
    if (artifacts.heightfield == nullptr ||
        !rcCreateHeightfield(
            &context,
            *artifacts.heightfield,
            config.width,
            config.height,
            config.bmin,
            config.bmax,
            config.cs,
            config.ch)) {
        *error = "Recast heightfield creation failed";
        return false;
    }
    std::vector<unsigned char> triangle_areas(triangle_count, 0);
    rcMarkWalkableTriangles(
        &context,
        config.walkableSlopeAngle,
        positions.data(),
        vertex_count,
        indices.data(),
        triangle_count,
        triangle_areas.data());
    if (!rcRasterizeTriangles(
            &context,
            positions.data(),
            vertex_count,
            indices.data(),
            triangle_areas.data(),
            triangle_count,
            *artifacts.heightfield,
            config.walkableClimb)) {
        *error = "Recast rasterization failed";
        return false;
    }
    rcFilterLowHangingWalkableObstacles(
        &context, config.walkableClimb, *artifacts.heightfield);
    rcFilterLedgeSpans(
        &context,
        config.walkableHeight,
        config.walkableClimb,
        *artifacts.heightfield);
    rcFilterWalkableLowHeightSpans(
        &context, config.walkableHeight, *artifacts.heightfield);

    artifacts.compact_heightfield = rcAllocCompactHeightfield();
    if (artifacts.compact_heightfield == nullptr ||
        !rcBuildCompactHeightfield(
            &context,
            config.walkableHeight,
            config.walkableClimb,
            *artifacts.heightfield,
            *artifacts.compact_heightfield) ||
        !rcErodeWalkableArea(
            &context,
            config.walkableRadius,
            *artifacts.compact_heightfield) ||
        !rcBuildDistanceField(&context, *artifacts.compact_heightfield) ||
        !rcBuildRegions(
            &context,
            *artifacts.compact_heightfield,
            0,
            config.minRegionArea,
            config.mergeRegionArea)) {
        *error = "Recast compact heightfield/region build failed";
        return false;
    }

    artifacts.contour_set = rcAllocContourSet();
    artifacts.poly_mesh = rcAllocPolyMesh();
    artifacts.detail_mesh = rcAllocPolyMeshDetail();
    if (artifacts.contour_set == nullptr || artifacts.poly_mesh == nullptr ||
        artifacts.detail_mesh == nullptr ||
        !rcBuildContours(
            &context,
            *artifacts.compact_heightfield,
            config.maxSimplificationError,
            config.maxEdgeLen,
            *artifacts.contour_set) ||
        !rcBuildPolyMesh(
            &context,
            *artifacts.contour_set,
            config.maxVertsPerPoly,
            *artifacts.poly_mesh) ||
        !rcBuildPolyMeshDetail(
            &context,
            *artifacts.poly_mesh,
            *artifacts.compact_heightfield,
            config.detailSampleDist,
            config.detailSampleMaxError,
            *artifacts.detail_mesh)) {
        *error = "Recast polygon/detail mesh build failed";
        return false;
    }
    for (int i = 0; i < artifacts.poly_mesh->npolys; ++i) {
        if (artifacts.poly_mesh->areas[i] == RC_WALKABLE_AREA) {
            artifacts.poly_mesh->areas[i] = 0;
        }
        artifacts.poly_mesh->flags[i] = 1;
    }

    dtNavMeshCreateParams params{};
    params.verts = artifacts.poly_mesh->verts;
    params.vertCount = artifacts.poly_mesh->nverts;
    params.polys = artifacts.poly_mesh->polys;
    params.polyAreas = artifacts.poly_mesh->areas;
    params.polyFlags = artifacts.poly_mesh->flags;
    params.polyCount = artifacts.poly_mesh->npolys;
    params.nvp = artifacts.poly_mesh->nvp;
    params.detailMeshes = artifacts.detail_mesh->meshes;
    params.detailVerts = artifacts.detail_mesh->verts;
    params.detailVertsCount = artifacts.detail_mesh->nverts;
    params.detailTris = artifacts.detail_mesh->tris;
    params.detailTriCount = artifacts.detail_mesh->ntris;
    params.walkableHeight = bake_config.recast_agent_height;
    params.walkableRadius = bake_config.recast_agent_radius;
    params.walkableClimb = bake_config.recast_agent_max_climb;
    rcVcopy(params.bmin, artifacts.poly_mesh->bmin);
    rcVcopy(params.bmax, artifacts.poly_mesh->bmax);
    params.cs = config.cs;
    params.ch = config.ch;
    params.buildBvTree = true;

    unsigned char* nav_data = nullptr;
    int nav_data_size = 0;
    if (!dtCreateNavMeshData(&params, &nav_data, &nav_data_size)) {
        *error = "Detour navmesh data creation failed";
        return false;
    }
    payload->assign(nav_data, nav_data + nav_data_size);
    dtFree(nav_data);
    return true;
}

bool initialize_detour_navmesh(
    const std::vector<std::uint8_t>& artifact,
    const ArtifactHeader& header,
    std::string* error) {
    auto* nav_data = static_cast<unsigned char*>(dtAlloc(
        static_cast<int>(header.payload_size), DT_ALLOC_PERM));
    if (nav_data == nullptr) {
        *error = "Detour navmesh allocation failed";
        return false;
    }
    std::memcpy(
        nav_data,
        artifact.data() + kMeshArtifactHeaderSize,
        static_cast<std::size_t>(header.payload_size));
    if (header.payload_endianness != native_endianness()) {
        if (!dtNavMeshHeaderSwapEndian(
                nav_data, static_cast<int>(header.payload_size)) ||
            !dtNavMeshDataSwapEndian(
                nav_data, static_cast<int>(header.payload_size))) {
            dtFree(nav_data);
            *error = "Detour navmesh endian conversion failed";
            return false;
        }
    }
    std::unique_ptr<dtNavMesh, decltype(&dtFreeNavMesh)> nav_mesh(
        dtAllocNavMesh(), dtFreeNavMesh);
    if (!nav_mesh ||
        dtStatusFailed(nav_mesh->init(
            nav_data,
            static_cast<int>(header.payload_size),
            DT_TILE_FREE_DATA))) {
        dtFree(nav_data);
        *error = "Detour navmesh initialization failed";
        return false;
    }
    std::unique_ptr<dtNavMeshQuery, decltype(&dtFreeNavMeshQuery)> query(
        dtAllocNavMeshQuery(), dtFreeNavMeshQuery);
    if (!query || dtStatusFailed(query->init(nav_mesh.get(), 128))) {
        *error = "Detour query initialization failed";
        return false;
    }
    const float range_x = header.bounds.max[0] - header.bounds.min[0];
    const float range_y = header.bounds.max[1] - header.bounds.min[1];
    const float range_z = header.bounds.max[2] - header.bounds.min[2];
    const float extents[3] = {
        std::max(2.0f, range_x),
        std::max(4.0f, range_y + 2.0f),
        std::max(2.0f, range_z),
    };
    const float start[3] = {
        header.bounds.min[0] + range_x * 0.1f,
        (header.bounds.min[1] + header.bounds.max[1]) * 0.5f,
        header.bounds.min[2] + range_z * 0.1f,
    };
    const float end[3] = {
        header.bounds.min[0] + range_x * 0.9f,
        (header.bounds.min[1] + header.bounds.max[1]) * 0.5f,
        header.bounds.min[2] + range_z * 0.9f,
    };
    dtQueryFilter filter;
    dtPolyRef start_ref = 0;
    dtPolyRef end_ref = 0;
    float nearest_start[3]{};
    float nearest_end[3]{};
    if (dtStatusFailed(query->findNearestPoly(
            start, extents, &filter, &start_ref, nearest_start)) ||
        dtStatusFailed(query->findNearestPoly(
            end, extents, &filter, &end_ref, nearest_end)) ||
        start_ref == 0 || end_ref == 0) {
        *error = "Detour nearest-poly query failed";
        return false;
    }
    std::array<dtPolyRef, 64> path{};
    int path_count = 0;
    if (dtStatusFailed(query->findPath(
            start_ref,
            end_ref,
            nearest_start,
            nearest_end,
            &filter,
            path.data(),
            &path_count,
            static_cast<int>(path.size()))) ||
        path_count == 0) {
        *error = "Detour path query failed";
        return false;
    }
    return true;
}

}  // namespace

MeshBakeConfig default_mesh_bake_config() {
    return MeshBakeConfig{};
}

MeshBakeConfigResult load_mesh_bake_config_file(const std::string& path) {
    MeshBakeConfigResult result;
    try {
        const YAML::Node root = YAML::LoadFile(path);
        if (!validate_keys(root, {"version", "jolt", "recast"}, "root", &result.error) ||
            !validate_keys(
                root["jolt"],
                {"max_triangles_per_leaf", "build_quality"},
                "jolt",
                &result.error) ||
            !validate_keys(
                root["recast"],
                {
                    "cell_size",
                    "cell_height",
                    "agent_height",
                    "agent_radius",
                    "agent_max_climb",
                    "walkable_slope_degrees",
                    "region_min_size",
                    "region_merge_size",
                    "edge_max_length",
                    "edge_max_error",
                    "max_vertices_per_polygon",
                    "detail_sample_distance_factor",
                    "detail_sample_max_error_factor",
                },
                "recast",
                &result.error)) {
            return result;
        }
        if (root["version"].as<std::uint32_t>() != 1) {
            result.error = "unsupported mesh bake config version";
            return result;
        }
        result.config.jolt_max_triangles_per_leaf =
            root["jolt"]["max_triangles_per_leaf"].as<std::uint32_t>();
        const std::string build_quality =
            root["jolt"]["build_quality"].as<std::string>();
        if (build_quality != "runtime_performance" &&
            build_quality != "build_speed") {
            result.error = "jolt.build_quality must be runtime_performance or build_speed";
            return result;
        }
        result.config.jolt_favor_runtime_performance =
            build_quality == "runtime_performance";
        const YAML::Node recast = root["recast"];
        result.config.recast_cell_size = recast["cell_size"].as<float>();
        result.config.recast_cell_height = recast["cell_height"].as<float>();
        result.config.recast_agent_height = recast["agent_height"].as<float>();
        result.config.recast_agent_radius = recast["agent_radius"].as<float>();
        result.config.recast_agent_max_climb = recast["agent_max_climb"].as<float>();
        result.config.recast_walkable_slope_degrees =
            recast["walkable_slope_degrees"].as<float>();
        result.config.recast_region_min_size = recast["region_min_size"].as<float>();
        result.config.recast_region_merge_size =
            recast["region_merge_size"].as<float>();
        result.config.recast_edge_max_length = recast["edge_max_length"].as<float>();
        result.config.recast_edge_max_error = recast["edge_max_error"].as<float>();
        result.config.recast_max_vertices_per_polygon =
            recast["max_vertices_per_polygon"].as<std::uint32_t>();
        result.config.recast_detail_sample_distance_factor =
            recast["detail_sample_distance_factor"].as<float>();
        result.config.recast_detail_sample_max_error_factor =
            recast["detail_sample_max_error_factor"].as<float>();
        validate_config(result.config, &result.error);
    } catch (const std::exception& exception) {
        result.error = std::string("failed to load mesh bake config: ") + exception.what();
    }
    return result;
}

std::uint64_t fnv1a_file_hash(const std::string& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *error = "cannot open file for hashing: " + path;
        return 0;
    }
    std::uint64_t hash = kFnvOffsetBasis;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), buffer.size());
        const std::streamsize count = input.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(i)]);
            hash *= kFnvPrime;
        }
    }
    if (!input.eof()) {
        *error = "failed while hashing file: " + path;
        return 0;
    }
    return hash;
}

MeshBakeResult bake_mesh_artifacts(
    const CanonicalTriangleMesh& mesh,
    const MeshBakeConfig& config,
    std::uint64_t source_hash,
    std::uint64_t config_hash) {
    MeshBakeResult result;
    if (!validate_config(config, &result.error)) {
        return result;
    }
    if (mesh.positions.size() > std::numeric_limits<std::uint32_t>::max() ||
        mesh.triangle_indices.size() / 3 >
            std::numeric_limits<std::uint32_t>::max()) {
        result.error = "mesh exceeds artifact count limits";
        return result;
    }
    std::vector<std::uint8_t> jolt_payload;
    if (!bake_jolt_payload(mesh, config, &jolt_payload, &result.error)) {
        return result;
    }
    std::vector<std::uint8_t> recast_payload;
    if (!bake_recast_payload(mesh, config, &recast_payload, &result.error)) {
        return result;
    }
    result.artifacts.jolt = wrap_artifact(
        kJoltMagic,
        jolt_version_id(),
        mesh,
        source_hash,
        config_hash,
        jolt_payload);
    result.artifacts.recast = wrap_artifact(
        kRecastMagic,
        DT_NAVMESH_VERSION,
        mesh,
        source_hash,
        config_hash,
        recast_payload);
    if (!validate_jolt_mesh_artifact(
            result.artifacts.jolt,
            mesh,
            source_hash,
            config_hash,
            &result.error) ||
        !validate_recast_mesh_artifact(
            result.artifacts.recast,
            source_hash,
            config_hash,
            &result.error)) {
        result.artifacts = {};
    }
    return result;
}

bool validate_jolt_mesh_artifact(
    const std::vector<std::uint8_t>& artifact,
    const CanonicalTriangleMesh& source_mesh,
    std::uint64_t expected_source_hash,
    std::uint64_t expected_config_hash,
    std::string* error) {
    ArtifactHeader header;
    if (!parse_artifact(
            artifact,
            kJoltMagic,
            jolt_version_id(),
            expected_source_hash,
            expected_config_hash,
            &header,
            error)) {
        return false;
    }
    if (header.payload_endianness != native_endianness()) {
        *error = "Jolt mesh artifact endianness mismatch";
        return false;
    }
    JoltRuntime runtime;
    if (!runtime.initialized()) {
        *error = "failed to initialize Jolt runtime";
        return false;
    }
    const std::string payload(
        reinterpret_cast<const char*>(artifact.data() + kMeshArtifactHeaderSize),
        static_cast<std::size_t>(header.payload_size));
    std::istringstream input(payload, std::ios::binary | std::ios::in);
    JPH::StreamInWrapper stream(input);
    JPH::Shape::IDToShapeMap shape_map;
    JPH::Shape::IDToMaterialMap material_map;
    JPH::Shape::ShapeResult shape_result = JPH::Shape::sRestoreWithChildren(
        stream, shape_map, material_map);
    if (shape_result.HasError()) {
        *error = "Jolt MeshShape restore failed: " + shape_result.GetError();
        return false;
    }

    BroadPhaseLayers broad_phase_layers;
    ObjectVsBroadPhaseFilter object_vs_broad_phase_filter;
    ObjectLayerPairFilter object_layer_pair_filter;
    JPH::PhysicsSystem physics_system;
    physics_system.Init(
        128,
        0,
        128,
        128,
        broad_phase_layers,
        object_vs_broad_phase_filter,
        object_layer_pair_filter);
    JPH::BodyInterface& body_interface = physics_system.GetBodyInterface();
    const JPH::BodyID body_id = body_interface.CreateAndAddBody(
        JPH::BodyCreationSettings(
            shape_result.Get(),
            JPH::RVec3::sZero(),
            JPH::Quat::sIdentity(),
            JPH::EMotionType::Static,
            kStaticLayer),
        JPH::EActivation::DontActivate);
    if (body_id.IsInvalid()) {
        *error = "Jolt restored static body creation failed";
        return false;
    }

    const std::uint32_t a = source_mesh.triangle_indices[0];
    const std::uint32_t b = source_mesh.triangle_indices[1];
    const std::uint32_t c = source_mesh.triangle_indices[2];
    const auto& va = source_mesh.positions[a];
    const auto& vb = source_mesh.positions[b];
    const auto& vc = source_mesh.positions[c];
    const std::array<float, 3> ab = {
        vb[0] - va[0], vb[1] - va[1], vb[2] - va[2]};
    const std::array<float, 3> ac = {
        vc[0] - va[0], vc[1] - va[1], vc[2] - va[2]};
    std::array<float, 3> normal = {
        ab[1] * ac[2] - ab[2] * ac[1],
        ab[2] * ac[0] - ab[0] * ac[2],
        ab[0] * ac[1] - ab[1] * ac[0],
    };
    const float length = std::sqrt(
        normal[0] * normal[0] + normal[1] * normal[1] +
        normal[2] * normal[2]);
    for (float& value : normal) {
        value /= length;
    }
    const std::array<float, 3> center = {
        (va[0] + vb[0] + vc[0]) / 3.0f,
        (va[1] + vb[1] + vc[1]) / 3.0f,
        (va[2] + vb[2] + vc[2]) / 3.0f,
    };
    const float distance = 1.0f;
    JPH::RayCastResult hit;
    const bool had_hit = physics_system.GetNarrowPhaseQuery().CastRay(
        JPH::RRayCast(
            JPH::RVec3(
                center[0] + normal[0] * distance,
                center[1] + normal[1] * distance,
                center[2] + normal[2] * distance),
            JPH::Vec3(
                -normal[0] * distance * 2.0f,
                -normal[1] * distance * 2.0f,
                -normal[2] * distance * 2.0f)),
        hit);
    body_interface.RemoveBody(body_id);
    body_interface.DestroyBody(body_id);
    if (!had_hit || hit.mBodyID != body_id) {
        *error = "Jolt restored MeshShape raycast failed";
        return false;
    }
    return true;
}

bool validate_recast_mesh_artifact(
    const std::vector<std::uint8_t>& artifact,
    std::uint64_t expected_source_hash,
    std::uint64_t expected_config_hash,
    std::string* error) {
    ArtifactHeader header;
    if (!parse_artifact(
            artifact,
            kRecastMagic,
            DT_NAVMESH_VERSION,
            expected_source_hash,
            expected_config_hash,
            &header,
            error)) {
        return false;
    }
    return initialize_detour_navmesh(artifact, header, error);
}

}  // namespace network_example
