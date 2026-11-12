// The level editor's registry (docs/LEVEL_AUTHORING_TOOLS_PLAN.md P4): the
// file the bundle build writes is exactly what the shipped bundle's catalog
// says -- every prop template by name with its id, its collider's box, and the
// hashes of that catalog and that bundle -- and nothing that is not a prop.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "game_server/src/gameplay_config.h"
#include "game_server/src/placeable_registry.h"

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "placeable_registry_test failed at line %d: %s\n", line, text);
        std::exit(1);
    }
}
#define require(condition) require_impl((condition), __LINE__, #condition)

std::string read_text(const char* path) {
    std::ifstream in(path, std::ios::binary);
    require(in.good());
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string entry_of(const std::string& json, const std::string& name) {
    const std::string key = "{\"name\": \"" + name + "\",";
    const std::size_t at = json.find(key);
    if (at == std::string::npos) return {};
    return json.substr(at, json.find('}', json.find("\"bounds\"", at)) - at + 2u);
}

}  // namespace

int main() {
    const std::string bundle_text = read_text("game_server/gameplay_catalog_bundle/bundle.zip");
    const std::vector<std::uint8_t> bundle(bundle_text.begin(), bundle_text.end());
    const gs::GameServerGameplayConfig config = gs::load_gameplay_config_from_bundle_memory(
        bundle.data(), static_cast<std::uint32_t>(bundle.size()), "gameplay_catalog.yaml");
    const std::string built = read_text("game_server/gameplay_catalog_bundle/placeable_registry.json");
    const std::string here = gs::placeable_registry_json(
        config, gs::bundle_bytes_hash(std::span<const std::uint8_t>(bundle)));
    // The build's file is this bundle's, byte for byte, and so is every rerun.
    require(built == here);
    require(here == gs::placeable_registry_json(
        config, gs::bundle_bytes_hash(std::span<const std::uint8_t>(bundle))));
    require(built.find("\"catalog_version\": " + std::to_string(config.weapons.catalog_version) + ",") !=
        std::string::npos);

    int props = 0;
    for (const gs::EntityTemplateConfig& t : config.entity_templates) {
        const std::string entry = entry_of(built, t.name);
        if (t.entity_type != KernelEntityType_Prop) {
            require(entry.empty());  // the player, agents and directors are not placed by hand
            continue;
        }
        ++props;
        require(!entry.empty());
        require(entry.find("\"id\": " + std::to_string(t.actor_template_id) + ",") != std::string::npos);
    }
    require(props > 0);

    // The nest's box is its hitbox collider's.
    const std::string nest = entry_of(built, "gingerbread_nest");
    require(!nest.empty());
    std::uint32_t nest_collider = 0;
    for (const gs::EntityTemplateConfig& t : config.entity_templates) {
        if (t.name == "gingerbread_nest") nest_collider = t.collider_template_id;
    }
    bool found = false;
    for (const gs::ColliderTemplateConfig& c : config.colliders.templates) {
        if (c.definition.template_id != nest_collider) continue;
        found = true;
        char want[96];
        std::snprintf(want, sizeof(want), "\"half_extents\": [%.4f, %.4f, %.4f]",
            static_cast<double>(c.definition.shape_params.x),
            static_cast<double>(c.definition.shape_params.y),
            static_cast<double>(c.definition.shape_params.z));
        std::printf("nest: %s\n", nest.c_str());
        require(nest.find(want) != std::string::npos);
    }
    require(found);
    for (const char* name : {"initial_camp", "level_terminal"}) {
        require(!entry_of(built, name).empty());
    }
    std::printf("placeable_registry_test passed: %d props\n", props);
    return 0;
}
