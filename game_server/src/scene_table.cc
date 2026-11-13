#include "game_server/src/scene_table.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <stdexcept>
#include <random>
#include <span>
#include <utility>

#include "game_server/src/scene_placement.h"
#include "game_server/src/terrain_catalog.h"

namespace network_example::game_server {
namespace {

void shift(KernelVec3* position, const KernelVec3& offset) {
    position->x += offset.x;
    position->y += offset.y;
    position->z += offset.z;
}

// A controller spec's world-space parts: a game rule's route and any guard
// anchor. (A world rule's route is offsets from the spawn; see the caller.)
void shift_controller(AgentControllerSpec* spec, const KernelVec3& offset, bool route_is_world) {
    if (route_is_world) {
        for (KernelVec3& point : spec->route) shift(&point, offset);
    }
    if (spec->has_anchor) shift(&spec->anchor, offset);
}

}  // namespace

KernelVec3 scene_slot_offset(std::uint32_t slot) {
    return KernelVec3{kSceneSlotSpacing * static_cast<float>(slot), 0.0f, 0.0f};
}

SceneBounds scene_slot_bounds(std::uint32_t slot) {
    const float center = kSceneSlotSpacing * static_cast<float>(slot);
    return SceneBounds{true, center - kSceneSlotSpacing * 0.5f, center + kSceneSlotSpacing * 0.5f};
}

std::uint32_t scene_slot_of(float x) {
    const float index = std::floor(x / kSceneSlotSpacing + 0.5f);
    if (!(index >= 0.0f) || index >= static_cast<float>(kSceneSlotCount)) {
        return kSceneSlotCount;
    }
    return static_cast<std::uint32_t>(index);
}

void offset_scene_config(GameServerGameplayConfig* config, std::uint32_t slot) {
    const KernelVec3 offset = scene_slot_offset(slot);
    config->scene_offset = offset;
    shift(&config->player.spawn_position, offset);
    shift(&config->level_gate.entrance.position, offset);
    shift(&config->level_gate.arrival.position, offset);
    for (KernelVec3& nest : config->level_nests.positions) shift(&nest, offset);
    for (ScenePropConfig& prop : config->scene_props) shift(&prop.position, offset);
    for (SceneItemConfig& item : config->scene_items) shift(&item.position, offset);
    for (AgentRouteConfig& route : config->agent_routes) {
        for (KernelVec3& point : route.waypoints) shift(&point, offset);
    }
    PlayableBoundsConfig& bounds = config->map.playable_bounds;
    if (bounds.authored) {
        bounds.min_x += offset.x;
        bounds.max_x += offset.x;
        bounds.min_z += offset.z;
        bounds.max_z += offset.z;
    }
    for (GameRuleConfig& rule : config->game_rules) {
        for (GameRuleNodeConfig& node : rule.nodes) {
            if (!node.has_spawn_effect) continue;
            shift(&node.spawn.position, offset);
            shift_controller(&node.spawn.controller, offset, true);
        }
    }
    for (WorldRuleSpawnConfig& rule : config->world_rule_spawns) {
        shift(&rule.position, offset);
        // A world rule's route is offsets from each agent's spawn point.
        shift_controller(&rule.controller, offset, false);
    }
    for (PatrolDefinitionConfig& patrol : config->patrols) {
        shift(&patrol.area.center, offset);
    }
}

float terrain_ground_height(const terrain::HeightGrid& heights, float x, float z) {
    const auto sample = [](float metres, std::int32_t samples) {
        const long index = std::lround((metres + static_cast<float>(samples - 1) * 0.15f) / 0.3f);
        return static_cast<std::int32_t>(std::clamp<long>(index, 0L, samples - 1L));
    };
    const std::int32_t i = sample(x, heights.samples_x);
    const std::int32_t j = sample(z, heights.samples_z);
    return static_cast<float>(heights.cm[static_cast<std::size_t>(j) * static_cast<std::size_t>(heights.samples_x) +
        static_cast<std::size_t>(i)]) / 100.0f;
}

KernelVec3 stamp_site_on_map(
    const terrain::TerrainRecipe& recipe,
    const terrain::TerrainPlacement& placement,
    const terrain::StampAsset& stamp,
    const StampSiteConfig& site) {
    const std::int32_t e = stamp.cells_x;
    const std::int32_t want_x = std::clamp(static_cast<std::int32_t>(std::lround(site.x / 0.3f)), 0, e);
    const std::int32_t want_z = std::clamp(static_cast<std::int32_t>(std::lround(site.z / 0.3f)), 0, e);
    // turn_source maps map samples to stamp samples; the way back is the
    // sample that maps to this one.
    std::int32_t found_u = 0;
    std::int32_t found_v = 0;
    for (std::int32_t v = 0; v <= e; ++v) {
        for (std::int32_t u = 0; u <= e; ++u) {
            std::int32_t lx = 0;
            std::int32_t lz = 0;
            terrain::turn_source(u, v, e, placement.turns, placement.mirror, &lx, &lz);
            if (lx == want_x && lz == want_z) {
                found_u = u;
                found_v = v;
            }
        }
    }
    return KernelVec3{
        static_cast<float>((placement.x_cells + found_u) * 0.3 - recipe.cells_x * 0.15),
        0.0f,
        static_cast<float>((placement.z_cells + found_v) * 0.3 - recipe.cells_z * 0.15),
    };
}

namespace {

// A portable generator (no std distribution, whose output differs between
// standard libraries): the same seed draws the same level everywhere, which is
// what seed_override replays.
struct SplitMix {
    std::uint64_t state;
    std::uint64_t next() {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    std::uint64_t below(std::uint64_t n) { return n == 0u ? 0u : next() % n; }
};

// Where a placed stamp's own frame lands on the map: its low corner's local
// point (x, z) goes to origin + x * along_x + z * along_z, and its border to
// base_y. Measured through stamp_site_on_map, so it is the recipe's own turn
// and mirror rule and never a second copy of it.
struct StampFrame {
    float centre = 0.0f;  // the stamp's local centre, on both axes
    KernelVec3 at_centre{};
    float along_x[2] = {1.0f, 0.0f};  // map (x, z) of local +x
    float along_z[2] = {0.0f, 1.0f};  // map (x, z) of local +z
    float base_y = 0.0f;
    bool mirror = false;
    float turn = 0.0f;  // radians about +y, after the mirror
};

StampFrame stamp_frame(
    const terrain::TerrainRecipe& recipe,
    const terrain::TerrainPlacement& placement,
    const terrain::StampAsset& stamp) {
    StampFrame frame;
    frame.centre = static_cast<float>(stamp.cells_x) * 0.15f;
    constexpr float kProbe = 3.0f;  // 10 cells: exact on the grid
    const KernelVec3 c = stamp_site_on_map(recipe, placement, stamp, StampSiteConfig{frame.centre, frame.centre});
    const KernelVec3 x = stamp_site_on_map(recipe, placement, stamp, StampSiteConfig{frame.centre + kProbe, frame.centre});
    const KernelVec3 z = stamp_site_on_map(recipe, placement, stamp, StampSiteConfig{frame.centre, frame.centre + kProbe});
    frame.at_centre = c;
    frame.along_x[0] = (x.x - c.x) / kProbe;
    frame.along_x[1] = (x.z - c.z) / kProbe;
    frame.along_z[0] = (z.x - c.x) / kProbe;
    frame.along_z[1] = (z.z - c.z) / kProbe;
    frame.base_y = static_cast<float>(placement.base_cm) / 100.0f;
    frame.mirror = frame.along_x[0] * frame.along_z[1] - frame.along_x[1] * frame.along_z[0] < 0.0f;
    // A mirror flips the stamp's x only, so local +z shows the turn alone.
    frame.turn = std::atan2(frame.along_z[0], frame.along_z[1]);
    return frame;
}

KernelVec3 on_map(const StampFrame& frame, const KernelVec3& local) {
    const float dx = local.x - frame.centre;
    const float dz = local.z - frame.centre;
    return KernelVec3{
        frame.at_centre.x + dx * frame.along_x[0] + dz * frame.along_z[0],
        frame.base_y + local.y,
        frame.at_centre.z + dx * frame.along_x[1] + dz * frame.along_z[1]};
}

KernelQuat multiply(const KernelQuat& a, const KernelQuat& b) {
    return KernelQuat{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

KernelQuat on_map(const StampFrame& frame, KernelQuat local) {
    if (frame.mirror) {
        // Reflected through the stamp's x: (x, y, z, w) -> (x, -y, -z, w).
        local = KernelQuat{local.x, -local.y, -local.z, local.w};
    }
    const KernelQuat turn{0.0f, std::sin(frame.turn * 0.5f), 0.0f, std::cos(frame.turn * 0.5f)};
    return multiply(turn, local);
}

float yaw_on_map(const StampFrame& frame, float yaw) {
    return frame.turn + (frame.mirror ? -yaw : yaw);
}

struct Limit {
    std::uint32_t min = 0;
    std::uint32_t max = 0xffffffffu;
};

// One site of one placed stamp, and the option it holds (-1: nothing).
struct Pick {
    std::size_t placement = 0;
    std::int32_t site = -1;  // -1: a stamp with no sites, holding a nest at its centre
    std::int32_t option = -1;
};

}  // namespace

// What the placed stamps put on the map (docs/LEVEL_AUTHORING_TOOLS_PLAN.md
// G4): every site draws one option, weighted, in a shuffled placement order so
// no stamp fills a ceiling first; per-map limits hold across the whole map, a
// short minimum is made up by switching sites that can hold the template; a
// level's nests keep their own ceiling. Nests go to level_nests, everything else
// to scene_props, and stamp barriers to player_barriers, all turned and
// mirrored with their stamp. Throws when a minimum cannot be met.
void place_stamp_contents(
    GameServerGameplayConfig* config,
    const terrain::HeightGrid& heights,
    const terrain::TerrainRecipe& recipe) {
    const TerrainCatalogConfig& terrain = config->terrain;
    LevelNestsConfig& nests = config->level_nests;
    const std::uint32_t nest = nests.entity_template_id;
    if (terrain.stamp_contents.size() != terrain.library.stamps.size()) {
        throw std::runtime_error("stamp contents and stamp library disagree");
    }

    std::map<std::uint32_t, Limit> limits;
    for (const terrain::TerrainPlacement& placement : recipe.placements) {
        for (const StampPlacementLimit& l : terrain.stamp_contents[placement.stamp].per_map_limits) {
            Limit& limit = limits[l.entity_template_id];
            limit.min = std::max(limit.min, l.min);
            limit.max = std::min(limit.max, l.max);
        }
    }
    if (nest != 0u) {
        Limit& limit = limits[nest];
        limit.max = std::min(limit.max, nests.max_count);
    }
    for (const auto& [id, limit] : limits) {
        if (limit.min > limit.max) {
            throw std::runtime_error("per_map_limits for entity template " + std::to_string(id) +
                " ask for at least " + std::to_string(limit.min) + " but at most " + std::to_string(limit.max));
        }
    }
    const auto max_of = [&](std::uint32_t id) {
        const auto found = limits.find(id);
        return found == limits.end() ? 0xffffffffu : found->second.max;
    };
    const auto min_of = [&](std::uint32_t id) {
        const auto found = limits.find(id);
        return found == limits.end() ? 0u : found->second.min;
    };

    SplitMix rng{static_cast<std::uint64_t>(static_cast<std::uint32_t>(recipe.seed)) * 0x2545f4914f6cdd1dull + 1u};
    std::vector<std::size_t> order(recipe.placements.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    for (std::size_t i = order.size(); i > 1u; --i) {
        std::swap(order[i - 1u], order[rng.below(i)]);
    }

    std::map<std::uint32_t, std::uint32_t> counts;
    std::vector<Pick> picks;
    const auto template_of = [&](const Pick& pick) -> std::uint32_t {
        if (pick.site < 0) return pick.option >= 0 ? nest : 0u;
        if (pick.option < 0) return 0u;
        const StampContentConfig& content = terrain.stamp_contents[recipe.placements[pick.placement].stamp];
        return content.sites[static_cast<std::size_t>(pick.site)]
            .options[static_cast<std::size_t>(pick.option)].entity_template_id;
    };
    for (const std::size_t index : order) {
        const StampContentConfig& content = terrain.stamp_contents[recipe.placements[index].stamp];
        if (!content.has_sites) {
            // As before sites existed: a level's nest at the stamp's centre.
            Pick pick{index, -1, -1};
            if (nest != 0u && counts[nest] < max_of(nest)) {
                pick.option = 0;
                ++counts[nest];
            }
            picks.push_back(pick);
            continue;
        }
        for (std::size_t s = 0; s < content.sites.size(); ++s) {
            const StampSiteDefinition& site = content.sites[s];
            std::uint64_t total = 0;
            for (const StampSiteOptionConfig& o : site.options) {
                if (o.entity_template_id == 0u || counts[o.entity_template_id] < max_of(o.entity_template_id)) {
                    total += o.weight;
                }
            }
            Pick pick{index, static_cast<std::int32_t>(s), -1};
            std::uint64_t roll = rng.below(total);
            for (std::size_t o = 0; total != 0u && o < site.options.size(); ++o) {
                const StampSiteOptionConfig& option = site.options[o];
                if (option.entity_template_id != 0u &&
                    counts[option.entity_template_id] >= max_of(option.entity_template_id)) {
                    continue;
                }
                if (roll < option.weight) {
                    pick.option = static_cast<std::int32_t>(o);
                    break;
                }
                roll -= option.weight;
            }
            const std::uint32_t chosen = template_of(pick);
            if (chosen != 0u) ++counts[chosen];
            picks.push_back(pick);
        }
    }

    // Minimums: switch sites that can hold the template, from what they drew,
    // as long as that does not take another template under its own minimum.
    for (const auto& [id, limit] : limits) {
        for (Pick& pick : picks) {
            if (counts[id] >= limit.min) break;
            if (pick.site < 0) continue;
            const std::uint32_t held = template_of(pick);
            if (held == id || (held != 0u && counts[held] <= min_of(held))) continue;
            const StampSiteDefinition& site = terrain.stamp_contents[recipe.placements[pick.placement].stamp]
                .sites[static_cast<std::size_t>(pick.site)];
            std::uint64_t total = 0;
            for (const StampSiteOptionConfig& o : site.options) {
                if (o.entity_template_id == id) total += o.weight;
            }
            if (total == 0u) continue;
            std::uint64_t roll = rng.below(total);
            for (std::size_t o = 0; o < site.options.size(); ++o) {
                if (site.options[o].entity_template_id != id) continue;
                if (roll < site.options[o].weight) {
                    if (held != 0u) --counts[held];
                    pick.option = static_cast<std::int32_t>(o);
                    ++counts[id];
                    break;
                }
                roll -= site.options[o].weight;
            }
        }
        if (counts[id] < limit.min) {
            throw std::runtime_error("per_map_limits: entity template " + std::to_string(id) + " needs " +
                std::to_string(limit.min) + " on the map, and its stamps hold only " + std::to_string(counts[id]));
        }
    }

    for (const Pick& pick : picks) {
        const terrain::TerrainPlacement& placement = recipe.placements[pick.placement];
        const terrain::StampAsset& stamp = terrain.library.stamps[placement.stamp];
        const StampFrame frame = stamp_frame(recipe, placement, stamp);
        const std::uint32_t id = template_of(pick);
        if (id == 0u) continue;
        if (pick.site < 0) {
            KernelVec3 at = frame.at_centre;
            at.y = terrain_ground_height(heights, at.x, at.z);
            nests.positions.push_back(at);
            continue;
        }
        const StampSiteOptionConfig& option = terrain.stamp_contents[placement.stamp]
            .sites[static_cast<std::size_t>(pick.site)].options[static_cast<std::size_t>(pick.option)];
        KernelVec3 at = on_map(frame, option.position);
        if (id == nest) {
            if (option.grounded) at.y = terrain_ground_height(heights, at.x, at.z);
            nests.positions.push_back(at);
            continue;
        }
        ScenePropConfig prop;
        prop.entity_template_id = id;
        prop.position = at;
        prop.rotation = on_map(frame, option.rotation);
        prop.grounded = option.grounded;  // finish_scene_placement grounds it
        config->scene_props.push_back(prop);
    }
    for (const terrain::TerrainPlacement& placement : recipe.placements) {
        const StampContentConfig& content = terrain.stamp_contents[placement.stamp];
        if (content.player_barriers.empty()) continue;
        const StampFrame frame = stamp_frame(recipe, placement, terrain.library.stamps[placement.stamp]);
        for (KernelPlayerBarrierBox box : content.player_barriers) {
            box.center = on_map(frame, box.center);
            box.yaw_radians = yaw_on_map(frame, box.yaw_radians);
            config->player_barriers.push_back(box);
        }
    }
}

void place_level_content(
    GameServerGameplayConfig* config,
    const terrain::HeightGrid& heights,
    const terrain::TerrainRecipe& recipe) {
    PlayerGameplayDefinition& player = config->player;
    if (!player.has_spawn_position) {
        player.has_spawn_position = true;
        player.spawn_position = KernelVec3{0.0f, terrain_ground_height(heights, 0.0f, 0.0f) + 1.5f, 0.0f};
    }
    LevelNestsConfig& nests = config->level_nests;
    nests.positions.clear();
    place_stamp_contents(config, heights, recipe);
}

PreparedScene prepare_scene(
    const SceneCatalogSource& source,
    const std::string& scene_template,
    std::uint32_t slot,
    TerrainRole role,
    std::span<const std::uint8_t> given_recipe) {
    PreparedScene out;
    try {
        if (slot == 0u || slot >= kSceneSlotCount) {
            out.error = "slot " + std::to_string(slot) + " is not a level slot";
            return out;
        }
        const bool from_bundle = source.bundle != nullptr && !source.bundle->empty();
        const std::uint8_t* bytes = from_bundle ? source.bundle->data() : nullptr;
        const auto size = from_bundle ? static_cast<std::uint32_t>(source.bundle->size()) : 0u;
        out.config = from_bundle
            ? load_scene_template_config_from_bundle_memory(bytes, size, source.entry_path, scene_template)
            : load_scene_template_config_from_catalog_file(source.catalog_file, scene_template);
        GameServerGameplayConfig& config = out.config;
        if (config.terrain.enabled) {
            std::vector<std::uint8_t> recipe(given_recipe.begin(), given_recipe.end());
            if (recipe.empty() && from_bundle) {
                recipe = read_cooked_terrain_recipe(bytes, size, config.terrain);
            }
            if (recipe.empty() && role == TerrainRole::kServer &&
                config.terrain.seed == kTerrainSeedRandom) {
                std::random_device device;
                config.terrain.seed = source.seed_override.has_value()
                    ? *source.seed_override
                    : static_cast<std::int32_t>(device() & 0x7fffffffu);
            }
            PreparedTerrain prepared;
            std::string error;
            if (!prepare_catalog_terrain(&config, std::span<const std::uint8_t>(recipe),
                    role, &prepared, &error)) {
                out.error = "scene template " + scene_template + " terrain: " + error;
                return out;
            }
            out.collision_artifact = std::move(prepared.collision_artifact);
            if (prepared.generated) {
                out.recipe_bytes = terrain::write_recipe(prepared.recipe);
            }
            place_level_content(&config, prepared.heights, prepared.recipe.recipe);
            out.terrain_heights = std::move(prepared.heights);
            out.terrain_recipe = std::move(prepared.recipe);
        } else {
            const std::string& scene = config.static_collision_scene.entry_path;
            const std::string& navmesh = config.navigation_mesh.entry_path;
            out.collision_artifact = from_bundle
                ? load_gameplay_bundle_entry_bytes(bytes, size, scene)
                : load_gameplay_catalog_file_entry_bytes(source.catalog_file, scene);
            if (!navmesh.empty()) {
                config.navigation_mesh.artifact = from_bundle
                    ? load_gameplay_bundle_entry_bytes(bytes, size, navmesh)
                    : load_gameplay_catalog_file_entry_bytes(source.catalog_file, navmesh);
            }
        }
        if (out.collision_artifact.empty()) {
            out.error = "scene template " + scene_template + " has no collision scene";
            return out;
        }
        // In the scene's own coordinates, before the slot's offset: the
        // ground is the template's, and the kernel adds the offset to the
        // barriers itself.
        const SceneGround ground = config.terrain.enabled
            ? SceneGround{&out.terrain_heights, {}}
            : SceneGround{nullptr, out.collision_artifact};
        std::string placement_error;
        if (!finish_scene_placement(&config, ground, &placement_error)) {
            out.error = "scene template " + scene_template + ": " + placement_error;
            return out;
        }
        offset_scene_config(&config, slot);
        out.ok = true;
    } catch (const std::exception& error) {
        out.error = "scene template " + scene_template + ": " + error.what();
    }
    return out;
}

std::vector<std::vector<std::uint8_t>> encode_scene_recipe_chunks(
    std::uint32_t slot,
    std::uint32_t generation,
    std::uint32_t transfer_id,
    std::span<const std::uint8_t> recipe) {
    std::vector<std::vector<std::uint8_t>> chunks;
    const std::size_t count = (recipe.size() + kSceneRecipeChunkBytes - 1u) / kSceneRecipeChunkBytes;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t begin = index * kSceneRecipeChunkBytes;
        const std::size_t length = std::min(kSceneRecipeChunkBytes, recipe.size() - begin);
        std::vector<std::uint8_t> body;
        body.reserve(13u + length);
        body.push_back(static_cast<std::uint8_t>(slot));
        const auto put32 = [&](std::uint32_t v) {
            for (int k = 0; k < 4; ++k) body.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        };
        put32(generation);
        put32(transfer_id);
        body.push_back(static_cast<std::uint8_t>(index));
        body.push_back(static_cast<std::uint8_t>(index >> 8));
        body.push_back(static_cast<std::uint8_t>(count));
        body.push_back(static_cast<std::uint8_t>(count >> 8));
        body.insert(body.end(), recipe.begin() + static_cast<std::ptrdiff_t>(begin),
            recipe.begin() + static_cast<std::ptrdiff_t>(begin + length));
        chunks.push_back(std::move(body));
    }
    return chunks;
}

bool SceneRecipeAssembler::accept(std::span<const std::uint8_t> body) {
    if (body.size() <= 13u) return false;
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(body[at]) | (static_cast<std::uint32_t>(body[at + 1]) << 8) |
            (static_cast<std::uint32_t>(body[at + 2]) << 16) | (static_cast<std::uint32_t>(body[at + 3]) << 24);
    };
    const std::uint32_t slot = body[0];
    const std::uint32_t generation = u32(1);
    const std::uint32_t transfer_id = u32(5);
    const std::uint32_t index = body[9] | (static_cast<std::uint32_t>(body[10]) << 8);
    const std::uint32_t count = body[11] | (static_cast<std::uint32_t>(body[12]) << 8);
    if (count == 0u || index >= count) return false;
    auto found = transfers_.find(slot);
    if (found != transfers_.end()) {
        const Transfer& current = found->second;
        if (generation < current.generation ||
            (generation == current.generation && transfer_id < current.transfer_id)) {
            return true;  // stale, not malformed
        }
    }
    Transfer& transfer = transfers_[slot];
    if (transfer.chunks.empty() || transfer.generation != generation ||
        transfer.transfer_id != transfer_id || transfer.chunks.size() != count) {
        transfer = Transfer{};
        transfer.generation = generation;
        transfer.transfer_id = transfer_id;
        transfer.chunks.resize(count);
    }
    std::vector<std::uint8_t>& chunk = transfer.chunks[index];
    if (!chunk.empty()) return true;  // a repeat
    chunk.assign(body.begin() + 13, body.end());
    ++transfer.received;
    if (transfer.received == count) {
        transfer.bytes.clear();
        for (const std::vector<std::uint8_t>& part : transfer.chunks) {
            transfer.bytes.insert(transfer.bytes.end(), part.begin(), part.end());
        }
    }
    return true;
}

const std::vector<std::uint8_t>* SceneRecipeAssembler::complete(
    std::uint32_t slot, std::uint32_t generation) const {
    const auto found = transfers_.find(slot);
    if (found == transfers_.end() || found->second.generation != generation ||
        found->second.chunks.empty() || found->second.received != found->second.chunks.size()) {
        return nullptr;
    }
    return &found->second.bytes;
}

void SceneRecipeAssembler::forget(std::uint32_t slot) {
    transfers_.erase(slot);
}

}  // namespace network_example::game_server
