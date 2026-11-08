#include "game_server/src/scene_table.h"

#include <cmath>
#include <exception>
#include <span>
#include <utility>

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
    config->patrols.clear();
}

PreparedScene prepare_scene(
    const SceneCatalogSource& source,
    const std::string& scene_template,
    std::uint32_t slot) {
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
            if (!from_bundle) {
                out.error = "scene template " + scene_template +
                    " is a terrain; its recipe is only in the bundle";
                return out;
            }
            const std::vector<std::uint8_t> recipe = read_cooked_terrain_recipe(bytes, size, config.terrain);
            PreparedTerrain prepared;
            std::string error;
            if (!prepare_catalog_terrain(&config, std::span<const std::uint8_t>(recipe),
                    TerrainRole::kServer, &prepared, &error)) {
                out.error = "scene template " + scene_template + " terrain: " + error;
                return out;
            }
            out.collision_artifact = std::move(prepared.collision_artifact);
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
        offset_scene_config(&config, slot);
        out.ok = true;
    } catch (const std::exception& error) {
        out.error = "scene template " + scene_template + ": " + error.what();
    }
    return out;
}

}  // namespace network_example::game_server
