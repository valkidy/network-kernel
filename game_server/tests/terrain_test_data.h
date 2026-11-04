// Test data for the terrain tests: the four stamps and ground_0001 exported
// from unity-terrain-example (claude/stamp-export 7a775cc, StampExporter /
// GroundMapExporter), and Compose Test Map's seed 1 layout with its three
// heightfield stages (ComposeParityExporter), zipped.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>
#include <zip.h>

#include "game_server/src/terrain/terrain_assets.h"
#include "game_server/src/terrain/terrain_compose.h"

namespace terrain_test {

namespace terrain = network_example::game_server::terrain;

inline void require_impl(bool condition, int line, const char* text) {
    if (condition) {
        return;
    }
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}

#define require(expr) ::terrain_test::require_impl(static_cast<bool>(expr), __LINE__, #expr)

inline constexpr const char* kDir = "game_server/tests/testdata/terrain/";
inline const char* const kStampNames[] = {
    "24x24_Fortress_0001", "24x24_Fortress_0002", "24x24_L_concave_0001", "36x36_Depression_0001"};

inline std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream s(path, std::ios::binary);
    require(s.good());
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(s)), std::istreambuf_iterator<char>());
}

inline std::vector<std::uint8_t> read_zip_entry(const std::string& zip_path, const std::string& name) {
    struct zip_t* z = zip_open(zip_path.c_str(), 0, 'r');
    require(z != nullptr);
    require(zip_entry_open(z, name.c_str()) == 0);
    void* buf = nullptr;
    size_t size = 0;
    require(zip_entry_read(z, &buf, &size) >= 0);
    std::vector<std::uint8_t> out(static_cast<std::uint8_t*>(buf), static_cast<std::uint8_t*>(buf) + size);
    std::free(buf);
    zip_entry_close(z);
    zip_close(z);
    return out;
}

inline terrain::TerrainLibrary load_library() {
    terrain::TerrainLibrary lib;
    for (const char* name : kStampNames) {
        terrain::StampAsset s;
        std::string error;
        require(terrain::parse_stamp(read_file(std::string(kDir) + name + ".hf"), name, &s, &error));
        lib.stamps.push_back(std::move(s));
    }
    terrain::GroundMapAsset g;
    std::string error;
    require(terrain::parse_ground_map(read_file(std::string(kDir) + "ground_0001.nkgm"), "ground_0001", &g, &error));
    lib.ground_maps.push_back(std::move(g));
    return lib;
}

inline std::int32_t mm(double metres) { return static_cast<std::int32_t>(std::lround(metres * 1000.0)); }

// The Unity seed 1 layout as a recipe. Trigonometry happens here, in the test
// (the server's layout does it in P2); the recipe itself is integers.
inline terrain::TerrainRecipe parity_recipe(const terrain::TerrainLibrary& lib) {
    const std::vector<std::uint8_t> bytes = read_zip_entry(std::string(kDir) + "parity_seed1.zip", "recipe_seed1.yaml");
    const YAML::Node y = YAML::Load(std::string(bytes.begin(), bytes.end()));
    terrain::TerrainRecipe r;
    r.seed = y["seed"].as<int>();
    r.cells_x = r.cells_z = mm(y["map_size_m"].as<double>()) / terrain::kCellMm;
    const YAML::Node t = y["terrace"];
    r.terrace.levels = t["levels"].as<int>();
    r.terrace.level_height_mm = mm(t["level_height_m"].as<double>());
    r.terrace.black_mm = mm(t["black_m"].as<double>());
    r.terrace.riser_run_mm = mm(t["level_height_m"].as<double>() / std::tan(t["riser_slope_deg"].as<double>() * M_PI / 180.0));
    r.terrace.cliff_width_mm = mm(t["cliff_width_m"].as<double>());
    r.terrace.cliff_share_q16 = std::lround(t["cliff_share"].as<double>() * 65536.0);
    r.detail.amplitude_mm = mm(y["detail"]["amplitude_m"].as<double>());
    r.detail.wavelength_mm = mm(y["detail"]["wavelength_m"].as<double>());
    r.ring_tan_q16 = std::lround(std::tan(y["layout"]["ring_slope_deg"].as<double>() * M_PI / 180.0) * 65536.0);
    std::map<std::string, std::uint32_t> index;
    for (std::uint32_t k = 0; k < lib.stamps.size(); ++k) {
        index[lib.stamps[k].name] = k;
    }
    for (const YAML::Node& p : y["placements"]) {
        terrain::TerrainPlacement tp;
        tp.stamp = index.at(p["stamp"].as<std::string>());
        tp.x_cells = mm(p["x_m"].as<double>()) / terrain::kCellMm;
        tp.z_cells = mm(p["z_m"].as<double>()) / terrain::kCellMm;
        tp.base_cm = static_cast<std::int32_t>(std::lround(p["base_m"].as<double>() * 100.0));
        tp.ring_mm = mm(p["ring_m"].as<double>());
        tp.turns = static_cast<std::uint8_t>(p["turns"].as<int>());
        tp.mirror = p["mirror"].as<bool>();
        r.placements.push_back(tp);
    }
    return r;
}

inline terrain::HeightGrid parity_stage(const std::string& file) {
    terrain::HeightGrid g;
    std::string error;
    require(terrain::parse_height_grid(read_zip_entry(std::string(kDir) + "parity_seed1.zip", file), &g, &error));
    return g;
}

}  // namespace terrain_test
