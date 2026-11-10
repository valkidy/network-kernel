// Stamp sites (docs/LEVEL_AUTHORING_TOOLS_PLAN.md P3), over many seeds of the
// shipping random level: every site draws one weighted option, per-map limits
// hold across the whole map, a short minimum is made up, the draw replays from
// the seed, and what a site holds lands -- position, facing and barriers --
// where the recipe's turn and mirror really put the stamp.
//
// The shipping stamps author no sites yet, so the test gives every stamp the
// same content in memory and calls place_stamp_contents on the real composed
// map.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "game_server/src/scene_placement.h"
#include "game_server/src/scene_table.h"

namespace {

namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (!condition) {
        std::fprintf(stderr, "stamp_sites_test failed at line %d: %s\n", line, text);
        std::exit(1);
    }
}
#define require(condition) require_impl((condition), __LINE__, #condition)

constexpr const char* kEntry = "gameplay_catalog.yaml";
constexpr const char* kLevel = "level_random";
constexpr int kSeeds = 20;

std::vector<std::uint8_t> read_bundle() {
    std::ifstream in("game_server/gameplay_catalog_bundle/bundle.zip", std::ios::binary);
    require(in.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::uint32_t template_id(const gs::GameServerGameplayConfig& config, const std::string& name) {
    for (const gs::EntityTemplateConfig& t : config.entity_templates) {
        if (t.name == name) return t.actor_template_id;
    }
    require(false);
    return 0u;
}

KernelQuat yaw(float degrees) {
    const float half = degrees * 3.14159265f / 360.0f;
    return KernelQuat{0.0f, std::sin(half), 0.0f, std::cos(half)};
}

// +z turned by q, on the map's x and z.
void forward_of(const KernelQuat& q, float* x, float* z) {
    // v' = q v q*, for v = (0, 0, 1).
    *x = 2.0f * (q.x * q.z + q.w * q.y);
    *z = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
}

struct Ids {
    std::uint32_t nest = 0;
    std::uint32_t tower = 0;  // stands in for a building: any prop
    std::uint32_t gate = 0;   // stands in for the entrance: one per map
};

// The content every stamp gets: a courtyard that is empty, a nest or a
// "tower" (2(g)-1), and a gate that is one of two spots facing two ways
// (2(g)-2), at most one gate and at least two towers per map, and a barrier.
gs::StampContentConfig content_for(float edge, const Ids& ids) {
    gs::StampContentConfig content;
    content.has_sites = true;
    const float c = edge * 0.5f;

    gs::StampSiteDefinition courtyard;
    courtyard.name = "courtyard";
    for (const auto& [weight, id] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
             {10u, 0u}, {20u, ids.nest}, {70u, ids.tower}}) {
        gs::StampSiteOptionConfig o;
        o.weight = weight;
        o.entity_template_id = id;
        o.position = KernelVec3{c, 0.0f, c};
        o.rotation = yaw(90.0f);
        o.grounded = true;
        courtyard.options.push_back(o);
    }
    content.sites.push_back(courtyard);

    gs::StampSiteDefinition gate;
    gate.name = "gate";
    gs::StampSiteOptionConfig west;
    west.entity_template_id = ids.gate;
    west.position = KernelVec3{3.0f, 6.0f, c};  // 6 m over the border, not grounded
    west.rotation = yaw(0.0f);
    west.grounded = false;
    gs::StampSiteOptionConfig east = west;
    east.position = KernelVec3{edge - 3.0f, 6.0f, c};
    east.rotation = yaw(180.0f);
    gate.options = {west, east};
    content.sites.push_back(gate);

    content.per_map_limits.push_back(gs::StampPlacementLimit{"gate", ids.gate, 0u, 1u});
    content.per_map_limits.push_back(gs::StampPlacementLimit{"tower", ids.tower, 2u, 0xffffffffu});
    content.player_barriers.push_back(KernelPlayerBarrierBox{
        KernelVec3{c, 2.0f, 3.0f}, KernelVec3{4.0f, 2.0f, 0.5f}, 0.0f});
    return content;
}

struct Drawn {
    std::vector<gs::ScenePropConfig> props;
    std::vector<KernelVec3> nests;
    std::vector<KernelPlayerBarrierBox> barriers;
};

Drawn draw(gs::GameServerGameplayConfig config, const gs::terrain::HeightGrid& heights,
           const gs::terrain::TerrainRecipe& recipe) {
    config.scene_props.clear();
    config.player_barriers.clear();
    config.level_nests.positions.clear();
    gs::place_stamp_contents(&config, heights, recipe);
    return Drawn{config.scene_props, config.level_nests.positions, config.player_barriers};
}

bool same(const Drawn& a, const Drawn& b) {
    if (a.props.size() != b.props.size() || a.nests.size() != b.nests.size()) return false;
    for (std::size_t i = 0; i < a.props.size(); ++i) {
        if (a.props[i].entity_template_id != b.props[i].entity_template_id ||
            a.props[i].position.x != b.props[i].position.x ||
            a.props[i].position.z != b.props[i].position.z) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.nests.size(); ++i) {
        if (a.nests[i].x != b.nests[i].x || a.nests[i].z != b.nests[i].z) return false;
    }
    return true;
}

}  // namespace

int main() {
    auto bundle = std::make_shared<const std::vector<std::uint8_t>>(read_bundle());
    int towers = 0, gates = 0, nests = 0, empties = 0, facings = 0, mirrored = 0;
    for (int seed = 1; seed <= kSeeds; ++seed) {
        gs::SceneCatalogSource source;
        source.bundle = bundle;
        source.entry_path = kEntry;
        source.seed_override = seed;
        // Slot 0 coordinates: what place_level_content sees, before any offset.
        gs::PreparedScene scene = gs::prepare_scene(source, kLevel, 1u);
        if (!scene.ok) std::fprintf(stderr, "seed %d: %s\n", seed, scene.error.c_str());
        require(scene.ok);
        const gs::terrain::TerrainRecipe& recipe = scene.terrain_recipe.recipe;
        const gs::terrain::HeightGrid& heights = scene.terrain_heights;
        gs::GameServerGameplayConfig config = scene.config;

        Ids ids;
        ids.nest = config.level_nests.entity_template_id;
        require(ids.nest != 0u);
        ids.tower = template_id(config, "initial_camp");
        ids.gate = template_id(config, "level_terminal");
        for (std::size_t s = 0; s < config.terrain.stamp_contents.size(); ++s) {
            const float edge = static_cast<float>(config.terrain.library.stamps[s].cells_x) * 0.3f;
            config.terrain.stamp_contents[s] = content_for(edge, ids);
        }

        const Drawn drawn = draw(config, heights, recipe);
        // The same seed draws the same map.
        require(same(drawn, draw(config, heights, recipe)));

        // Limits: one gate at most, two towers at least, the level's nest ceiling.
        std::map<std::uint32_t, int> count;
        for (const gs::ScenePropConfig& prop : drawn.props) ++count[prop.entity_template_id];
        require(count[ids.gate] <= 1);
        require(count[ids.tower] >= 2);
        require(drawn.nests.size() <= config.level_nests.max_count);
        // Every stamp's courtyard holds exactly one thing or nothing.
        const int courtyard_picks = count[ids.tower] + static_cast<int>(drawn.nests.size());
        require(courtyard_picks <= static_cast<int>(recipe.placements.size()));
        empties += static_cast<int>(recipe.placements.size()) - courtyard_picks;
        towers += count[ids.tower];
        gates += count[ids.gate];
        nests += static_cast<int>(drawn.nests.size());

        // Positions and facings, against the stamp's own mapping of points.
        for (const gs::terrain::TerrainPlacement& p : recipe.placements) {
            const gs::terrain::StampAsset& stamp = config.terrain.library.stamps[p.stamp];
            const float c = static_cast<float>(stamp.cells_x) * 0.15f;
            const KernelVec3 at_c = gs::stamp_site_on_map(recipe, p, stamp, gs::StampSiteConfig{c, c});
            const KernelVec3 ahead = gs::stamp_site_on_map(recipe, p, stamp, gs::StampSiteConfig{c, c + 3.0f});
            const KernelVec3 right = gs::stamp_site_on_map(recipe, p, stamp, gs::StampSiteConfig{c + 3.0f, c});
            const KernelVec3 west = gs::stamp_site_on_map(recipe, p, stamp, gs::StampSiteConfig{3.0f, c});
            const KernelVec3 east = gs::stamp_site_on_map(recipe, p, stamp,
                gs::StampSiteConfig{static_cast<float>(stamp.cells_x) * 0.3f - 3.0f, c});
            if (p.mirror) ++mirrored;
            for (const gs::ScenePropConfig& prop : drawn.props) {
                if (prop.entity_template_id == ids.tower &&
                    std::fabs(prop.position.x - at_c.x) < 0.01f && std::fabs(prop.position.z - at_c.z) < 0.01f) {
                    // Local yaw 90 faces local +x: its facing is where the
                    // stamp's +x went.
                    float fx = 0.0f, fz = 0.0f;
                    forward_of(prop.rotation, &fx, &fz);
                    require(std::fabs(fx - (right.x - at_c.x) / 3.0f) < 1e-3f);
                    require(std::fabs(fz - (right.z - at_c.z) / 3.0f) < 1e-3f);
                    require(prop.grounded);
                    ++facings;
                }
                if (prop.entity_template_id == ids.gate) {
                    const bool at_west = std::fabs(prop.position.x - west.x) < 0.01f && std::fabs(prop.position.z - west.z) < 0.01f;
                    const bool at_east = std::fabs(prop.position.x - east.x) < 0.01f && std::fabs(prop.position.z - east.z) < 0.01f;
                    if (!at_west && !at_east) continue;  // another stamp's gate
                    // Not grounded: 6 m over the stamp's border, as authored.
                    require(std::fabs(prop.position.y - (static_cast<float>(p.base_cm) / 100.0f + 6.0f)) < 1e-3f);
                    // West faces local +z; east is turned a half turn, local -z.
                    float fx = 0.0f, fz = 0.0f;
                    forward_of(prop.rotation, &fx, &fz);
                    const float sign = at_west ? 1.0f : -1.0f;
                    require(std::fabs(fx - sign * (ahead.x - at_c.x) / 3.0f) < 1e-3f);
                    require(std::fabs(fz - sign * (ahead.z - at_c.z) / 3.0f) < 1e-3f);
                    ++facings;
                }
            }
        }
        // Barriers: one per stamp, centred where the stamp put (c, 3), with
        // its long side along where the stamp's +x went.
        require(drawn.barriers.size() == recipe.placements.size());
        for (std::size_t i = 0; i < recipe.placements.size(); ++i) {
            const gs::terrain::TerrainPlacement& p = recipe.placements[i];
            const gs::terrain::StampAsset& stamp = config.terrain.library.stamps[p.stamp];
            const float c = static_cast<float>(stamp.cells_x) * 0.15f;
            const KernelVec3 want = gs::stamp_site_on_map(recipe, p, stamp, gs::StampSiteConfig{c, 3.0f});
            const KernelVec3 at_c = gs::stamp_site_on_map(recipe, p, stamp, gs::StampSiteConfig{c, c});
            const KernelVec3 right = gs::stamp_site_on_map(recipe, p, stamp, gs::StampSiteConfig{c + 3.0f, c});
            const KernelPlayerBarrierBox& box = drawn.barriers[i];
            require(std::fabs(box.center.x - want.x) < 0.01f && std::fabs(box.center.z - want.z) < 0.01f);
            require(std::fabs(box.center.y - (static_cast<float>(p.base_cm) / 100.0f + 2.0f)) < 1e-3f);
            // The box's local +x, turned by its yaw about +y.
            const float lx = std::cos(box.yaw_radians);
            const float lz = -std::sin(box.yaw_radians);
            const float rx = (right.x - at_c.x) / 3.0f;
            const float rz = (right.z - at_c.z) / 3.0f;
            require(std::fabs(std::fabs(lx * rx + lz * rz) - 1.0f) < 1e-3f);
        }

        // Grounded picks stand on the ground once the scene is finished.
        gs::GameServerGameplayConfig finished = config;
        finished.scene_props = drawn.props;
        std::string error;
        require(gs::finish_scene_placement(&finished, gs::SceneGround{&heights, {}}, &error));
        for (const gs::ScenePropConfig& prop : finished.scene_props) {
            if (!prop.grounded) continue;
            require(std::fabs(prop.position.y - gs::terrain_ground_height(heights, prop.position.x, prop.position.z)) < 1e-3f);
        }
        for (const KernelVec3& nest : drawn.nests) {
            require(std::fabs(nest.y - gs::terrain_ground_height(heights, nest.x, nest.z)) < 1e-3f);
        }
        std::printf("seed %2d: %zu stamps, towers %d, gates %d, nests %zu\n", seed, recipe.placements.size(),
            count[ids.tower], count[ids.gate], drawn.nests.size());

        // A minimum the map cannot hold is refused, not quietly short.
        gs::GameServerGameplayConfig greedy = config;
        greedy.terrain.stamp_contents[0].per_map_limits.push_back(
            gs::StampPlacementLimit{"tower", ids.tower, 1000u, 0xffffffffu});
        bool threw = false;
        try {
            draw(greedy, heights, recipe);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        // Only when the first library stamp is on the map does its limit apply.
        bool stamp0_placed = false;
        for (const gs::terrain::TerrainPlacement& p : recipe.placements) stamp0_placed |= p.stamp == 0u;
        require(threw == stamp0_placed);
    }
    std::printf("%d seeds: towers %d, gates %d, nests %d, empty courtyards %d; %d facings checked, %d mirrored placements\n",
        kSeeds, towers, gates, nests, empties, facings, mirrored);
    // The draw is not stuck on one answer: every outcome happened somewhere.
    require(towers > 0 && gates > 0 && nests > 0 && empties > 0);
    require(facings > 0 && mirrored > 0);
    std::printf("stamp_sites_test passed\n");
    return 0;
}
