#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "geometry/public/mesh_bake.h"
#include "geometry/public/obj_importer.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace {

bool rejected_jolt_version(
    const network_example::MeshBakeArtifacts& artifacts,
    const network_example::CanonicalTriangleMesh& mesh,
    std::uint64_t source_hash,
    std::uint64_t config_hash) {
    std::vector<std::uint8_t> corrupted = artifacts.jolt;
    corrupted[network_example::kMeshArtifactBackendVersionOffset] ^= 1;
    std::string error;
    return !network_example::validate_jolt_mesh_artifact(
        corrupted, mesh, source_hash, config_hash, &error);
}

bool rejected_detour_version(
    const network_example::MeshBakeArtifacts& artifacts,
    std::uint64_t source_hash,
    std::uint64_t config_hash) {
    std::vector<std::uint8_t> corrupted = artifacts.recast;
    corrupted[network_example::kMeshArtifactHeaderSize + 4] ^= 1;
    std::string error;
    return !network_example::validate_recast_mesh_artifact(
        corrupted, source_hash, config_hash, &error);
}

bool rejected_truncated_payload(
    const network_example::MeshBakeArtifacts& artifacts,
    std::uint64_t source_hash,
    std::uint64_t config_hash) {
    std::vector<std::uint8_t> truncated = artifacts.recast;
    truncated.pop_back();
    std::string error;
    return !network_example::validate_recast_mesh_artifact(
        truncated, source_hash, config_hash, &error);
}

}  // namespace

int main(int argc, char** argv) {
    std::string input_path;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        constexpr std::string_view kInputPrefix = "--input=";
        if (argument.starts_with(kInputPrefix) && input_path.empty()) {
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

    const network_example::ObjImportResult imported =
        network_example::import_obj_file(input_path);
    if (!imported) {
        std::cerr << imported.error << "\nstatic_mesh_bake_smoke: FAIL\n";
        return 1;
    }
    std::string error;
    const std::uint64_t source_hash =
        network_example::fnv1a_file_hash(input_path, &error);
    constexpr std::uint64_t kDefaultConfigHash = 0x6e6b6d6573680001ull;
    network_example::MeshBakeResult baked =
        network_example::bake_mesh_artifacts(
            imported.mesh,
            network_example::default_mesh_bake_config(),
            source_hash,
            kDefaultConfigHash);
    if (!error.empty() || !baked) {
        std::cerr << (error.empty() ? baked.error : error)
                  << "\nstatic_mesh_bake_smoke: FAIL\n";
        return 1;
    }

    std::string hash_error;
    const bool hash_mismatch_rejected =
        !network_example::validate_jolt_mesh_artifact(
            baked.artifacts.jolt,
            imported.mesh,
            source_hash + 1,
            kDefaultConfigHash,
            &hash_error);
    if (!rejected_jolt_version(
            baked.artifacts, imported.mesh, source_hash, kDefaultConfigHash) ||
        !rejected_detour_version(
            baked.artifacts, source_hash, kDefaultConfigHash) ||
        !rejected_truncated_payload(
            baked.artifacts, source_hash, kDefaultConfigHash) ||
        !hash_mismatch_rejected) {
        std::cerr << "artifact corruption validation failed\n"
                  << "static_mesh_bake_smoke: FAIL\n";
        return 1;
    }

    std::cout << "Input vertices: " << imported.mesh.positions.size()
              << ", triangles: " << imported.mesh.triangle_indices.size() / 3
              << '\n'
              << "Jolt artifact bytes: " << baked.artifacts.jolt.size() << '\n'
              << "Recast artifact bytes: " << baked.artifacts.recast.size() << '\n'
              << "static_mesh_bake_smoke: PASS\n";
    return 0;
}
