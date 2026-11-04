// Writes a terrain level's composed map as a Wavefront OBJ, for looking at it
// in Unity (or anything else) before the client draws terrain itself (P5).
//
//   terrain_obj_export <bundle.zip> <entry.yaml> <out.obj> [stride]
//
// The map is composed from the bundle's cooked recipe exactly as a client does
// (and refused the same way if it does not match). Vertices are in kernel world
// metres, map centre at the origin; stride takes every Nth 0.3 m sample
// (default 2 = 0.6 m). Triangles follow the collision's split (Jolt's
// HeightFieldShape diagonal), facing up.
//
// In Unity: Unity's OBJ importer flips x, so put the instance under a parent
// with scale.x = -1 (the same rule as every collision OBJ in this project).

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/terrain_catalog.h"

int main(int argc, char** argv) {
    namespace gs = network_example::game_server;
    if (argc < 4 || argc > 5) {
        std::fprintf(stderr, "usage: %s <bundle.zip> <entry.yaml> <out.obj> [stride]\n", argv[0]);
        return 2;
    }
    const int stride = argc == 5 ? std::atoi(argv[4]) : 2;
    if (stride < 1) {
        std::fprintf(stderr, "stride must be >= 1\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> bundle((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_bundle_memory(
            bundle.data(), static_cast<std::uint32_t>(bundle.size()), argv[2]);
        if (!config.terrain.enabled) {
            std::fprintf(stderr, "%s has no terrain\n", argv[2]);
            return 1;
        }
        const std::vector<std::uint8_t> cooked =
            gs::read_cooked_terrain_recipe(bundle.data(), static_cast<std::uint32_t>(bundle.size()), config.terrain);
        gs::PreparedTerrain prepared;
        std::string error;
        if (!gs::prepare_catalog_terrain(&config, cooked, gs::TerrainRole::kClient, &prepared, &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        const gs::terrain::HeightGrid& g = prepared.heights;
        const int nx = (g.samples_x - 1) / stride + 1, nz = (g.samples_z - 1) / stride + 1;
        const double ox = -(g.samples_x - 1) * 0.15, oz = -(g.samples_z - 1) * 0.15;
        std::FILE* f = std::fopen(argv[3], "wb");
        if (f == nullptr) {
            std::fprintf(stderr, "cannot write %s\n", argv[3]);
            return 1;
        }
        std::fprintf(f, "# %s seed %d heights %016llx, %d x %d samples every %.1f m, kernel world metres\n", argv[2],
            prepared.recipe.recipe.seed, static_cast<unsigned long long>(prepared.recipe.heights_hash), nx, nz, 0.3 * stride);
        for (int j = 0; j < nz; ++j)
            for (int i = 0; i < nx; ++i)
                std::fprintf(f, "v %.2f %.2f %.2f\n", ox + i * stride * 0.3, g.at(i * stride, j * stride) / 100.0, oz + j * stride * 0.3);
        auto v = [nx](int i, int j) { return j * nx + i + 1; };
        for (int j = 0; j + 1 < nz; ++j)
            for (int i = 0; i + 1 < nx; ++i) {
                std::fprintf(f, "f %d %d %d\n", v(i, j), v(i, j + 1), v(i + 1, j + 1));
                std::fprintf(f, "f %d %d %d\n", v(i, j), v(i + 1, j + 1), v(i + 1, j));
            }
        std::fclose(f);
        std::printf("%s: %d vertices, %d triangles (heights %016llx)\n", argv[3], nx * nz, 2 * (nx - 1) * (nz - 1),
            static_cast<unsigned long long>(prepared.recipe.heights_hash));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
