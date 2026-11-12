// Writes the level editor's registry of placeable templates at bundle build
// time (docs/LEVEL_AUTHORING_TOOLS_PLAN.md P4): loads <entry> from
// <bundle.zip> and writes placeable_registry_json to <out.json>. Made from the
// finished bundle in the same build, so the ids and hashes in it are exactly
// that bundle's.
//
//   placeable_registry <bundle.zip> <entry.yaml> <out.json>

#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/placeable_registry.h"

int main(int argc, char** argv) {
    namespace gs = network_example::game_server;
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <bundle.zip> <entry.yaml> <out.json>\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> bundle((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bundle.empty()) {
        std::fprintf(stderr, "placeable_registry: cannot read %s\n", argv[1]);
        return 1;
    }
    try {
        const gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_bundle_memory(
            bundle.data(), static_cast<std::uint32_t>(bundle.size()), argv[2]);
        const std::string json = gs::placeable_registry_json(
            config, gs::bundle_bytes_hash(std::span<const std::uint8_t>(bundle)));
        std::ofstream out(argv[3], std::ios::binary | std::ios::trunc);
        out << json;
        if (!out.good()) {
            std::fprintf(stderr, "placeable_registry: cannot write %s\n", argv[3]);
            return 1;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "placeable_registry: %s\n", error.what());
        return 1;
    }
    return 0;
}
