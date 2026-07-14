#include <Jolt/Jolt.h>

#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <Recast.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "geometry/public/obj_importer.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace {

constexpr JPH::ObjectLayer kStaticLayer = 0;
constexpr JPH::BroadPhaseLayer kStaticBroadPhaseLayer(0);

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
 public:
  std::uint32_t GetNumBroadPhaseLayers() const override { return 1; }

  JPH::BroadPhaseLayer GetBroadPhaseLayer(
      JPH::ObjectLayer /*layer*/) const override {
    return kStaticBroadPhaseLayer;
  }
};

class ObjectVsBroadPhaseFilter final
    : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer /*layer1*/,
                     JPH::BroadPhaseLayer /*layer2*/) const override {
    return true;
  }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer /*layer1*/,
                     JPH::ObjectLayer /*layer2*/) const override {
    return true;
  }
};

struct RecastArtifacts {
  rcHeightfield* heightfield = nullptr;
  rcCompactHeightfield* compact_heightfield = nullptr;
  rcContourSet* contour_set = nullptr;
  rcPolyMesh* poly_mesh = nullptr;
  rcPolyMeshDetail* detail_mesh = nullptr;
  dtNavMesh* nav_mesh = nullptr;
  dtNavMeshQuery* nav_query = nullptr;

  ~RecastArtifacts() {
    dtFreeNavMeshQuery(nav_query);
    dtFreeNavMesh(nav_mesh);
    rcFreePolyMeshDetail(detail_mesh);
    rcFreePolyMesh(poly_mesh);
    rcFreeContourSet(contour_set);
    rcFreeCompactHeightfield(compact_heightfield);
    rcFreeHeightField(heightfield);
  }
};

bool run_jolt_smoke(const network_example::CanonicalTriangleMesh& mesh) {
  JPH::RegisterDefaultAllocator();
  JPH::Factory::sInstance = new JPH::Factory();
  JPH::RegisterTypes();

  bool passed = false;
  {
    JPH::TempAllocatorImpl temp_allocator(10 * 1024 * 1024);
    JPH::JobSystemThreadPool job_system(
        JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 1);
    BroadPhaseLayers broad_phase_layers;
    ObjectVsBroadPhaseFilter object_vs_broad_phase_filter;
    ObjectLayerPairFilter object_layer_pair_filter;
    JPH::PhysicsSystem physics_system;
    physics_system.Init(128, 0, 128, 128, broad_phase_layers,
                        object_vs_broad_phase_filter,
                        object_layer_pair_filter);

    JPH::VertexList vertices;
    vertices.reserve(mesh.positions.size());
    for (const auto& position : mesh.positions) {
      vertices.emplace_back(position[0], position[1], position[2]);
    }

    JPH::IndexedTriangleList triangles;
    triangles.reserve(mesh.triangle_indices.size() / 3);
    for (std::size_t i = 0; i < mesh.triangle_indices.size(); i += 3) {
      triangles.emplace_back(mesh.triangle_indices[i],
                             mesh.triangle_indices[i + 1],
                             mesh.triangle_indices[i + 2]);
    }

    JPH::MeshShapeSettings mesh_settings(std::move(vertices),
                                         std::move(triangles));
    JPH::ShapeSettings::ShapeResult shape_result = mesh_settings.Create();
    if (shape_result.HasError()) {
      std::cerr << "Jolt MeshShape creation failed: "
                << shape_result.GetError() << '\n';
    } else {
      JPH::ShapeRefC shape = shape_result.Get();
      JPH::BodyCreationSettings body_settings(
          shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
          JPH::EMotionType::Static, kStaticLayer);
      JPH::BodyInterface& body_interface = physics_system.GetBodyInterface();
      const JPH::BodyID body_id = body_interface.CreateAndAddBody(
          body_settings, JPH::EActivation::DontActivate);
      if (body_id.IsInvalid()) {
        std::cerr << "Jolt static body creation failed\n";
      } else {
        JPH::RayCastResult hit;
        const float center_x = (mesh.bounds.min[0] + mesh.bounds.max[0]) * 0.5f;
        const float center_z = (mesh.bounds.min[2] + mesh.bounds.max[2]) * 0.5f;
        const float ray_height =
            std::max(10.0f, mesh.bounds.max[1] - mesh.bounds.min[1] + 10.0f);
        const bool had_hit = physics_system.GetNarrowPhaseQuery().CastRay(
            JPH::RRayCast(
                JPH::RVec3(center_x, mesh.bounds.max[1] + ray_height, center_z),
                JPH::Vec3(0.0f, -2.0f * ray_height, 0.0f)),
            hit);
        std::cout << "Jolt shape: created, raycast hit: "
                  << (had_hit ? "yes" : "no") << '\n';
        passed = had_hit && hit.mBodyID == body_id;
        body_interface.RemoveBody(body_id);
        body_interface.DestroyBody(body_id);
      }
    }
  }

  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;
  return passed;
}

bool run_recast_detour_smoke(
    const network_example::CanonicalTriangleMesh& mesh) {
  RecastArtifacts artifacts;
  rcContext context;
  rcConfig config{};
  config.cs = 0.3f;
  config.ch = 0.2f;
  config.walkableSlopeAngle = 45.0f;
  config.walkableHeight = static_cast<int>(std::ceil(2.0f / config.ch));
  config.walkableClimb = static_cast<int>(std::floor(0.9f / config.ch));
  config.walkableRadius = static_cast<int>(std::ceil(0.6f / config.cs));
  config.maxEdgeLen = static_cast<int>(12.0f / config.cs);
  config.maxSimplificationError = 1.3f;
  config.minRegionArea = 0;
  config.mergeRegionArea = 0;
  config.maxVertsPerPoly = 6;
  config.detailSampleDist = config.cs * 6.0f;
  config.detailSampleMaxError = config.ch;

  if (mesh.positions.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      mesh.triangle_indices.size() / 3 >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    std::cerr << "Mesh exceeds Recast index limits\n";
    return false;
  }
  std::vector<float> positions;
  positions.reserve(mesh.positions.size() * 3);
  for (const auto& position : mesh.positions) {
    positions.insert(positions.end(), position.begin(), position.end());
  }
  std::vector<int> indices;
  indices.reserve(mesh.triangle_indices.size());
  for (const std::uint32_t index : mesh.triangle_indices) {
    if (index > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
      std::cerr << "Mesh index exceeds Recast limits\n";
      return false;
    }
    indices.push_back(static_cast<int>(index));
  }
  const int vertex_count = static_cast<int>(mesh.positions.size());
  const int triangle_count = static_cast<int>(mesh.triangle_indices.size() / 3);
  rcCalcBounds(positions.data(), vertex_count, config.bmin, config.bmax);
  rcCalcGridSize(config.bmin, config.bmax, config.cs, &config.width,
                 &config.height);

  artifacts.heightfield = rcAllocHeightfield();
  if (artifacts.heightfield == nullptr ||
      !rcCreateHeightfield(&context, *artifacts.heightfield, config.width,
                           config.height, config.bmin, config.bmax, config.cs,
                           config.ch)) {
    std::cerr << "Recast heightfield creation failed\n";
    return false;
  }

  std::vector<unsigned char> triangle_areas(triangle_count, 0);
  rcMarkWalkableTriangles(&context, config.walkableSlopeAngle,
                          positions.data(), vertex_count, indices.data(),
                          triangle_count, triangle_areas.data());
  if (!rcRasterizeTriangles(&context, positions.data(), vertex_count,
                            indices.data(), triangle_areas.data(),
                            triangle_count, *artifacts.heightfield,
                            config.walkableClimb)) {
    std::cerr << "Recast rasterization failed\n";
    return false;
  }

  rcFilterLowHangingWalkableObstacles(&context, config.walkableClimb,
                                      *artifacts.heightfield);
  rcFilterLedgeSpans(&context, config.walkableHeight, config.walkableClimb,
                     *artifacts.heightfield);
  rcFilterWalkableLowHeightSpans(&context, config.walkableHeight,
                                 *artifacts.heightfield);

  artifacts.compact_heightfield = rcAllocCompactHeightfield();
  if (artifacts.compact_heightfield == nullptr ||
      !rcBuildCompactHeightfield(&context, config.walkableHeight,
                                 config.walkableClimb, *artifacts.heightfield,
                                 *artifacts.compact_heightfield) ||
      !rcErodeWalkableArea(&context, config.walkableRadius,
                           *artifacts.compact_heightfield) ||
      !rcBuildDistanceField(&context, *artifacts.compact_heightfield) ||
      !rcBuildRegions(&context, *artifacts.compact_heightfield, 0,
                      config.minRegionArea, config.mergeRegionArea)) {
    std::cerr << "Recast compact heightfield/region build failed\n";
    return false;
  }

  artifacts.contour_set = rcAllocContourSet();
  artifacts.poly_mesh = rcAllocPolyMesh();
  artifacts.detail_mesh = rcAllocPolyMeshDetail();
  if (artifacts.contour_set == nullptr || artifacts.poly_mesh == nullptr ||
      artifacts.detail_mesh == nullptr ||
      !rcBuildContours(&context, *artifacts.compact_heightfield,
                       config.maxSimplificationError, config.maxEdgeLen,
                       *artifacts.contour_set) ||
      !rcBuildPolyMesh(&context, *artifacts.contour_set,
                       config.maxVertsPerPoly, *artifacts.poly_mesh) ||
      !rcBuildPolyMeshDetail(&context, *artifacts.poly_mesh,
                             *artifacts.compact_heightfield,
                             config.detailSampleDist,
                             config.detailSampleMaxError,
                             *artifacts.detail_mesh)) {
    std::cerr << "Recast polygon/detail mesh build failed\n";
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
  params.walkableHeight = 2.0f;
  params.walkableRadius = 0.6f;
  params.walkableClimb = 0.9f;
  rcVcopy(params.bmin, artifacts.poly_mesh->bmin);
  rcVcopy(params.bmax, artifacts.poly_mesh->bmax);
  params.cs = config.cs;
  params.ch = config.ch;
  params.buildBvTree = true;

  unsigned char* nav_data = nullptr;
  int nav_data_size = 0;
  if (!dtCreateNavMeshData(&params, &nav_data, &nav_data_size)) {
    std::cerr << "Detour navmesh data creation failed\n";
    return false;
  }

  artifacts.nav_mesh = dtAllocNavMesh();
  if (artifacts.nav_mesh == nullptr ||
      dtStatusFailed(artifacts.nav_mesh->init(nav_data, nav_data_size,
                                              DT_TILE_FREE_DATA))) {
    dtFree(nav_data);
    std::cerr << "Detour navmesh initialization failed\n";
    return false;
  }

  artifacts.nav_query = dtAllocNavMeshQuery();
  if (artifacts.nav_query == nullptr ||
      dtStatusFailed(artifacts.nav_query->init(artifacts.nav_mesh, 128))) {
    std::cerr << "Detour query initialization failed\n";
    return false;
  }

  dtQueryFilter filter;
  const float range_x = params.bmax[0] - params.bmin[0];
  const float range_y = params.bmax[1] - params.bmin[1];
  const float range_z = params.bmax[2] - params.bmin[2];
  const float extents[3] = {
      std::max(2.0f, range_x),
      std::max(4.0f, range_y + 2.0f),
      std::max(2.0f, range_z),
  };
  const float start[3] = {
      params.bmin[0] + range_x * 0.1f,
      (params.bmin[1] + params.bmax[1]) * 0.5f,
      params.bmin[2] + range_z * 0.1f,
  };
  const float end[3] = {
      params.bmin[0] + range_x * 0.9f,
      (params.bmin[1] + params.bmax[1]) * 0.5f,
      params.bmin[2] + range_z * 0.9f,
  };
  float nearest_start[3]{};
  float nearest_end[3]{};
  dtPolyRef start_ref = 0;
  dtPolyRef end_ref = 0;
  if (dtStatusFailed(artifacts.nav_query->findNearestPoly(
          start, extents, &filter, &start_ref, nearest_start)) ||
      dtStatusFailed(artifacts.nav_query->findNearestPoly(
          end, extents, &filter, &end_ref, nearest_end)) ||
      start_ref == 0 || end_ref == 0) {
    std::cerr << "Detour nearest-poly query failed\n";
    return false;
  }

  std::array<dtPolyRef, 64> path{};
  int path_count = 0;
  if (dtStatusFailed(artifacts.nav_query->findPath(
          start_ref, end_ref, nearest_start, nearest_end, &filter, path.data(),
          &path_count, static_cast<int>(path.size()))) ||
      path_count == 0) {
    std::cerr << "Detour path query failed\n";
    return false;
  }

  std::cout << "Recast polygons: " << artifacts.poly_mesh->npolys
            << ", detail triangles: " << artifacts.detail_mesh->ntris << '\n'
            << "Detour navmesh bytes: " << nav_data_size
            << ", path polygons: " << path_count << '\n';
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string input_path;
  for (int i = 1; i < argc; ++i) {
    const std::string argument(argv[i]);
    constexpr std::string_view kInputPrefix = "--input=";
    if (argument.starts_with(kInputPrefix)) {
      input_path = argument.substr(kInputPrefix.size());
    } else {
      std::cerr << "usage: static_mesh_bake_smoke [--input=<obj path>]\n";
      return 2;
    }
  }
  if (input_path.empty()) {
    std::string runfiles_error;
    std::unique_ptr<bazel::tools::cpp::runfiles::Runfiles> runfiles(
        bazel::tools::cpp::runfiles::Runfiles::Create(argv[0], &runfiles_error));
    if (!runfiles) {
      std::cerr << "Runfiles initialization failed: " << runfiles_error << '\n';
      return 2;
    }
    input_path = runfiles->Rlocation(
        "recastnavigation/RecastDemo/Bin/Meshes/dungeon.obj");
  }

  network_example::ObjImportResult imported =
      network_example::import_obj_file(input_path);
  if (!imported) {
    std::cerr << imported.error << "\nstatic_mesh_bake_smoke: FAIL\n";
    return 1;
  }
  const auto& mesh = imported.mesh;
  std::cout << "Input vertices: " << mesh.positions.size()
            << ", triangles: " << mesh.triangle_indices.size() / 3 << '\n'
            << "Bounds: [" << mesh.bounds.min[0] << ", " << mesh.bounds.min[1]
            << ", " << mesh.bounds.min[2] << "] - [" << mesh.bounds.max[0]
            << ", " << mesh.bounds.max[1] << ", " << mesh.bounds.max[2]
            << "]\n";

  const bool jolt_passed = run_jolt_smoke(mesh);
  const bool navigation_passed = run_recast_detour_smoke(mesh);
  if (!jolt_passed || !navigation_passed) {
    std::cerr << "static_mesh_bake_smoke: FAIL\n";
    return 1;
  }
  std::cout << "static_mesh_bake_smoke: PASS\n";
  return 0;
}
