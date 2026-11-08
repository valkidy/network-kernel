#ifndef GAME_SERVER_GAME_SERVER_H_
#define GAME_SERVER_GAME_SERVER_H_

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game_server/src/agent_runtime_manager.h"
#include "game_server/src/gameplay_config.h"
#include "game_server/src/respawn_scheduler.h"
#include "game_server/src/scene_table.h"
#include "game_server/src/loadout_director.h"
#include "game_server/src/shelter_director.h"
#include "game_server/public/game_server_types.h"
#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

// A level slot's life on the server (docs/SCENE_TABLE_PLAN.md §4.2). S3 has
// the server side only: Empty, Loading (prepared on a worker thread), Ready
// (in the kernel, its directors running). The client handshake, Active and
// Draining arrive with the load protocol (S4).
enum class SceneSlotState : std::uint8_t {
    kEmpty = 0,
    kLoading = 1,
    kReady = 2,
};

class GameServer {
public:
    explicit GameServer(
        KernelHandle* kernel,
        GameServerGameplayConfig config = default_game_server_gameplay_config());

    // The loadout director holds this object's config and a callback into it,
    // so it never moves.
    GameServer(const GameServer&) = delete;
    GameServer& operator=(const GameServer&) = delete;

    void handle_event(const KernelEvent& event);
    void tick(float delta_seconds);
    bool preload_directors();

    AgentRuntimeManager& agent_runtime_manager();
    const AgentRuntimeManager& agent_runtime_manager() const;
    const ShelterDirector& shelter_director() const { return shelter_; }
    const LoadoutDirector& loadout_director() const { return loadout_; }
    bool query_weapon_template(
        std::uint8_t weapon_id,
        GameServerWeaponTemplateInfo* out_info) const;

    // The scene table (docs/SCENE_TABLE_PLAN.md S3). Until it is enabled the
    // server is what it always was: one map, unbounded. Enabling it confines
    // the camp's AI to slot 0 and names where level templates load from.
    void enable_scene_table(SceneCatalogSource source);
    bool scene_table_enabled() const { return scene_table_enabled_; }
    // Starts loading `scene_template` into an empty level slot (1 and up);
    // the map is prepared on a worker thread and committed on a later tick.
    // False for slot 0, past the table, a slot not empty, or before
    // enable_scene_table.
    bool load_scene(std::uint32_t slot, const std::string& scene_template);
    // Empties a Ready level slot at once: players inside go back to the
    // camp's spawn, every other entity inside is removed (Retired), its
    // directors stop and its terrain leaves the kernel; the generation moves
    // on. False for anything not Ready.
    bool unload_scene(std::uint32_t slot);
    SceneSlotState scene_state(std::uint32_t slot) const;
    std::uint32_t scene_generation(std::uint32_t slot) const;
    // Why the last load of `slot` failed, empty if it did not.
    const std::string& scene_error(std::uint32_t slot) const;
    // The AI of a slot: the camp's for 0, a Ready level's own, else null.
    const AgentRuntimeManager* scene_manager(std::uint32_t slot) const;
    // A Ready level's config (in world space), else null.
    const GameServerGameplayConfig* scene_config(std::uint32_t slot) const;

private:
    // Health, weapons, ammo and starting inventory from the player template.
    // On join an existing inventory is kept; a revive (reset_inventory) empties
    // it and hands out the starting items again.
    void configure_player(std::uint32_t net_id, bool reset_inventory = false) const;
    bool configure_player_inventory(
        std::uint32_t net_id,
        const ActorTemplateConfig& actor_template,
        bool reset_inventory) const;
    // The player's weapon container, filled from its loadout's weapons or the
    // template default's weapon items; refilled fresh when `reset`.
    bool configure_player_weapons(std::uint32_t net_id, bool reset) const;
    void revive_player(std::uint32_t net_id, float delta_seconds);
    // A dead player's tagged items -- quest items, map weapons -- go to the
    // ground around them (D19, K11); the untagged stay for the respawn to
    // replace.
    void drop_tagged_items(std::uint32_t net_id) const;
    // A temporary camp that just appeared gets its stock (D8, K9): a
    // container it owns, one slot per camp.stock entry. Anything else
    // spawned is left alone.
    void stock_camp(std::uint32_t net_id) const;
    // Puts the catalog's scene_props in the world, once the kernel takes
    // them; tried every tick until it has.
    void place_scene_props();
    // Places `config`'s scene props and items; false when the kernel refused
    // the first prop (not serving yet), having placed nothing.
    bool place_props_of(const GameServerGameplayConfig& config) const;
    // Commits finished loads and runs the Ready levels' AI.
    void tick_scene_slots(float delta_seconds);
    bool commit_scene(std::uint32_t slot, PreparedScene prepared);
    // Every non-player entity whose position lies in `bounds` is removed;
    // every player there is put at the camp's spawn. Returns how many were
    // removed.
    std::uint32_t clear_scene_bounds(const SceneBounds& bounds) const;

    KernelHandle* kernel_ = nullptr;
    GameServerGameplayConfig config_;
    AgentRuntimeManager agent_runtime_manager_;
    RespawnScheduler respawn_;
    ShelterDirector shelter_;
    LoadoutDirector loadout_;
    std::set<std::uint32_t> players_;
    bool scene_props_placed_ = false;
    // Players carry their weapons as items when every weapon of the player
    // template's default loadout has a weapon item (P3); these are those
    // items, in loadout order. Otherwise players keep the template loadout.
    bool weapons_are_items_ = false;
    std::vector<std::uint32_t> default_weapon_items_;

    struct SceneSlot {
        SceneSlotState state = SceneSlotState::kEmpty;
        std::uint32_t generation = 0;
        std::string scene_template;
        std::string error;
        std::future<PreparedScene> pending;
        std::unique_ptr<GameServerGameplayConfig> config;
        // Holds `config` by value; built once the slot is committed.
        std::unique_ptr<AgentRuntimeManager> manager;
    };
    bool scene_table_enabled_ = false;
    SceneCatalogSource scene_source_;
    std::array<SceneSlot, kSceneSlotCount> scene_slots_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_GAME_SERVER_H_
