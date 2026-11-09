#ifndef GAME_SERVER_GAME_SERVER_H_
#define GAME_SERVER_GAME_SERVER_H_

#include <array>
#include <cstdint>
#include <future>
#include <map>
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

    // The party's way through levels (docs/SCENE_TABLE_PLAN.md S4.3): one
    // party, every player, level slot kPartyLevelSlot. From the camp a level
    // is asked for (request_level, or SCENE_SELECT from a listen host's own
    // player); it loads; once every player has reported SCENE_READY (or 60 s
    // have passed) a gate opens beside the camp spawn and a ready player who
    // walks into it arrives at the level's spawn. The mission completing opens
    // a gate back beside the level spawn; when the last player has gone
    // through it the level unloads. The party running out of revives with
    // every player in the level dead fails it: they come back to the camp
    // alive and the level unloads. Every change is broadcast as SCENE_STATE.
    static constexpr std::uint32_t kPartyLevelSlot = 1u;
    struct SceneGate {
        bool open = false;
        KernelVec3 position{0.0f, 0.0f, 0.0f};
        float radius = 2.0f;
    };
    // False unless the party is in the camp and the template exists.
    bool request_level(const std::string& scene_template);
    // Development only (a dedicated server's --level): whenever the party is
    // in the camp and someone is connected, open this level. Stops once a
    // load has failed.
    void set_auto_level(std::string scene_template) { auto_level_ = std::move(scene_template); }
    // Who may open and abort levels; see SCENE_STATE's owner_net_id.
    std::uint32_t level_owner_net_id() const;
    std::uint8_t scene_flow() const { return flow_; }
    std::uint8_t scene_outcome() const { return flow_outcome_; }
    const SceneGate& scene_gate() const { return gate_; }
    // Players who reported SCENE_READY for the party's current level.
    std::size_t scene_ready_count() const { return level_ready_.size(); }
    std::int32_t remaining_revives() const { return respawn_.remaining_revives(); }
    // Enemies defeated in the party's current level.
    std::uint32_t level_defeated() const { return level_defeated_; }

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
    void handle_scene_message(const KernelGameMessage& message);
    void tick_scene_flow(float delta_seconds);
    std::vector<std::uint8_t> encode_scene_state() const;
    // Marks every peer as owed the state and sends what it can; a send that
    // fails stays owed and is tried again next tick.
    void broadcast_scene_state();
    void mark_scene_state_owed();
    void flush_scene_recipes();
    // An enemy agent of the party's level died: count it towards the objective.
    void note_level_enemy_death(std::uint32_t net_id);
    // Someone joined after the party entered: their share of the revive pool.
    void grant_late_joiner_revives();
    bool level_objective_met() const;
    // A generated level's nests go up when the party first arrives.
    void place_level_nests();
    void flush_scene_state();
    // Everyone inside the level back at the camp spawn, the dead revived.
    void return_level_players_to_camp(const std::vector<KernelServerEntityState>& players);
    void set_flow(std::uint8_t flow);
    // Ends the party's level: back to the camp's revives, the slot unloaded.
    void finish_level(std::uint8_t outcome);
    std::uint32_t host_player_net_id() const;
    // The manager a command's agent belongs to: a level's, or the camp's.
    AgentRuntimeManager& manager_for_agent(std::uint32_t net_id);

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
        // The recipe when the server generated this map; clients fetch it.
        std::vector<std::uint8_t> recipe;
        std::uint64_t recipe_hash = 0;
    };
    bool scene_table_enabled_ = false;
    SceneCatalogSource scene_source_;
    std::array<SceneSlot, kSceneSlotCount> scene_slots_;

    std::uint8_t flow_ = GAME_SERVER_SCENE_FLOW_CAMP;
    std::uint8_t flow_outcome_ = GAME_SERVER_SCENE_OUTCOME_NONE;
    std::uint16_t level_template_id_ = 0;
    SceneGate gate_;
    // Player net id of each peer, for messages; peer 1 is a listen host's own.
    std::map<std::uint32_t, std::uint32_t> peer_players_;
    std::set<std::uint32_t> level_ready_;
    float ready_wait_seconds_ = 0.0f;
    bool level_entered_ = false;
    bool abort_level_requested_ = false;
    std::uint32_t level_defeated_ = 0;
    bool level_announced_ = false;
    std::set<std::uint32_t> level_failed_;
    std::set<std::uint32_t> recipe_owed_peers_;
    std::uint32_t next_recipe_transfer_id_ = 1;
    std::int32_t level_revives_granted_ = 0;
    float gate_open_seconds_ = 0.0f;
    std::string auto_level_;
    std::set<std::uint32_t> scene_state_owed_;
    float scene_state_refresh_seconds_ = 0.0f;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_GAME_SERVER_H_
