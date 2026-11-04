// Cooks a gray ground map (NKGM, 1.2 m) into terrace levels on the 0.3 m grid
// (NKGL) with compose's own conversion, so the catalog can ship the small form
// of the same map (game_server/src/terrain/terrain_assets.h).
//
//   terrain_ground_levels <in.nkgm> <levels> <out.nkgl>
//
// <levels> must be the terrace level count of the catalog that uses it.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "game_server/src/terrain/terrain_assets.h"
#include "game_server/src/terrain/terrain_compose.h"

int main(int argc, char** argv) {
    namespace terrain = network_example::game_server::terrain;
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <in.nkgm> <levels> <out.nkgl>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    terrain::GroundMapAsset gray;
    std::string error;
    if (!terrain::parse_ground_map(bytes, "ground", &gray, &error) || gray.is_levels()) {
        std::fprintf(stderr, "%s: %s\n", argv[1], gray.is_levels() ? "already levels" : error.c_str());
        return 1;
    }
    const int levels = std::atoi(argv[2]);
    if (levels < 1 || levels > 255) {
        std::fprintf(stderr, "levels must be 1-255\n");
        return 2;
    }
    const std::vector<std::uint8_t> out = terrain::write_nkgl(terrain::ground_levels_from_gray(gray, levels));
    std::ofstream o(argv[3], std::ios::binary);
    o.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!o) {
        std::fprintf(stderr, "cannot write %s\n", argv[3]);
        return 1;
    }
    std::printf("%s -> %s: %zu bytes\n", argv[1], argv[3], out.size());
    return 0;
}
