// P4a of docs/AI_CONTROLLER_PLAN.md: `ai.skills` and sp_attack_templates/.
// Every agent names what it attacks with as skills; leap and charge mechanics
// live in sp_attack templates, and the band and cooldown on the skill using
// one. The loader resolves skills into the weapon, attack range, leap and
// charge the controllers read -- the shipped catalog resolves to exactly what
// it read before (checked field by field when it was converted) -- and refuses
// what P4a cannot run yet or that cannot work at all.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include <yaml-cpp/yaml.h>

#include "game_server/src/gameplay_config.h"

namespace {

namespace fs = std::filesystem;
namespace gs = network_example::game_server;

void require_impl(bool condition, int line, const char* text) {
    if (condition) return;
    std::fprintf(stderr, "require failed at line %d: %s\n", line, text);
    std::abort();
}
#define require(expr) require_impl(static_cast<bool>(expr), __LINE__, #expr)

fs::path catalog_root() {
    const char* test_srcdir = std::getenv("TEST_SRCDIR");
    const char* test_workspace = std::getenv("TEST_WORKSPACE");
    require(test_srcdir != nullptr && test_workspace != nullptr);
    return fs::path(test_srcdir) / test_workspace / "game_server" / "gameplay_catalog";
}

gs::GameServerGameplayConfig load(const fs::path& root) {
    return gs::load_gameplay_config_from_catalog_file((root / "gameplay_catalog.yaml").string());
}

// Edits one catalog file in a copy; reports whether the copy loads.
bool loads_after(const char* label, const std::string& relative,
                 const std::function<void(YAML::Node&)>& edit) {
    const fs::path root = fs::path(std::getenv("TEST_TMPDIR")) / "edited";
    fs::remove_all(root);
    fs::copy(catalog_root(), root, fs::copy_options::recursive);
    const fs::path file = root / relative;
    YAML::Node node = YAML::LoadFile(file.string());
    edit(node);
    YAML::Emitter out;
    out << node;
    std::ofstream(file) << out.c_str() << "\n";
    try {
        (void)load(root);
        std::printf("[loads] %s\n", label);
        return true;
    } catch (const std::exception& error) {
        std::printf("[refused] %s: %s\n", label, error.what());
        return false;
    }
}

const char* kGingerbread = "entity_templates/28_gingerbread.yaml";
const char* kSentry = "entity_templates/2_sentry_grunt.yaml";
const char* kLeap = "sp_attack_templates/1_sp_attack_gingerbread_leap.yaml";

void the_catalog_resolves(const gs::GameServerGameplayConfig& config) {
    require(config.sp_attack_templates.size() == 2u);
    const gs::ActorTemplateConfig* gingerbread = gs::find_actor_template(config, 28u);
    require(gingerbread != nullptr && gingerbread->skills.size() == 2u);
    require(gingerbread->sentry.weapon_id == 10u);
    require(gingerbread->chaser.attack_range_meters == 2.2f);
    // The jump from the template, the band and cooldown from the skill.
    require(gingerbread->chaser.leap.apex_meters == 4.9f);
    require(gingerbread->chaser.leap.attack_lead_ticks == 9u);
    require(gingerbread->chaser.leap.min_distance_meters == 10.5f);
    require(gingerbread->chaser.leap.max_distance_meters == 24.0f);
    require(gingerbread->chaser.leap.cooldown_ticks == 90u);
    const gs::ActorTemplateConfig* warrior = gs::find_actor_template(config, 29u);
    require(warrior != nullptr);
    require(warrior->chaser.charge.weapon_id == 20u);
    require(warrior->chaser.charge.windup_ticks == 18u);
    require(warrior->chaser.charge.max_distance_meters == 18.0f);
    require(warrior->chaser.charge.cooldown_ticks == 150u);
    const gs::ActorTemplateConfig* sentry = gs::find_actor_template(config, 2u);
    require(sentry != nullptr && sentry->sentry.weapon_id == 2u);
    require(sentry->chaser.attack_range_meters == 0.0f);
}

void bad_skills_are_refused() {
    // Control: a valid edit loads, so each refusal below is the edit's.
    require(loads_after("narrower claw range", kGingerbread, [](YAML::Node& root) {
        root["ai"]["skills"][0]["when"]["range_meters"]["max"] = 2.15;
    }));
    require(!loads_after("no skills", kGingerbread, [](YAML::Node& root) {
        root["ai"].remove("skills");
    }));
    require(!loads_after("old attack_range_meters", kGingerbread, [](YAML::Node& root) {
        root["ai"]["locomotion"]["attack_range_meters"] = 2.2;
    }));
    require(!loads_after("old locomotion.leap", kGingerbread, [](YAML::Node& root) {
        root["ai"]["locomotion"]["leap"]["apex_meters"] = 4.9;
    }));
    require(!loads_after("weapon not in loadout", kGingerbread, [](YAML::Node& root) {
        root["ai"]["skills"][0]["use"]["value"] = 11;
    }));
    // P4b: a chaser may carry several weapon skills, with minimum ranges and
    // cooldowns, as long as one reaches from where it stops.
    require(loads_after("two weapons", kGingerbread, [](YAML::Node& root) {
        YAML::Node second;
        second["use"]["type"] = "weapon";
        second["use"]["value"] = 10;
        second["when"]["range_meters"]["min"] = 1.0;
        second["when"]["range_meters"]["max"] = 2.2;
        second["cooldown_ticks"] = 30;
        second["weight"] = 3;
        root["ai"]["skills"].push_back(second);
    }));
    require(!loads_after("nothing reaches from the stop distance", kGingerbread,
                         [](YAML::Node& root) {
        root["ai"]["skills"][0]["when"]["range_meters"]["min"] = 2.0;
        root["ai"]["skills"][0]["when"]["range_meters"]["max"] = 6.0;
    }));
    require(!loads_after("sentry with two weapons", kSentry, [](YAML::Node& root) {
        YAML::Node second;
        second["use"]["type"] = "weapon";
        second["use"]["value"] = 2;
        root["ai"]["skills"].push_back(second);
    }));
    require(!loads_after("min past max", kGingerbread, [](YAML::Node& root) {
        root["ai"]["skills"][1]["when"]["range_meters"]["min"] = 30.0;
    }));
    require(!loads_after("unknown sp_attack", kGingerbread, [](YAML::Node& root) {
        root["ai"]["skills"][1]["use"]["value"] = "gingerbread_flip";
    }));
    require(!loads_after("sp_attack with no band", kGingerbread, [](YAML::Node& root) {
        root["ai"]["skills"][1].remove("when");
    }));
    // P5: items, on conditions.
    const auto item_skill = [](const char* item) {
        YAML::Node skill;
        skill["use"]["type"] = "item";
        skill["use"]["value"] = item;
        return skill;
    };
    require(loads_after("item on entering chasing", kGingerbread, [&](YAML::Node& root) {
        YAML::Node skill = item_skill("fungible_hover_potion");
        skill["when"]["on_state_enter"] = "chasing";
        root["ai"]["skills"].push_back(skill);
    }));
    require(loads_after("item once below a third of health", kGingerbread, [&](YAML::Node& root) {
        YAML::Node skill = item_skill("fungible_hover_potion");
        skill["when"]["hp_below"] = 0.3;
        skill["once"] = true;
        root["ai"]["skills"].push_back(skill);
    }));
    require(!loads_after("item with no when", kGingerbread, [&](YAML::Node& root) {
        root["ai"]["skills"].push_back(item_skill("fungible_hover_potion"));
    }));
    require(!loads_after("item every tick it is chasing", kGingerbread, [&](YAML::Node& root) {
        YAML::Node skill = item_skill("fungible_hover_potion");
        skill["when"]["state"] = "chasing";
        root["ai"]["skills"].push_back(skill);
    }));
    require(!loads_after("unknown item", kGingerbread, [&](YAML::Node& root) {
        YAML::Node skill = item_skill("fungible_elixir_of_nothing");
        skill["when"]["on_spawn"] = true;
        root["ai"]["skills"].push_back(skill);
    }));
    require(!loads_after("weapon item", kGingerbread, [&](YAML::Node& root) {
        YAML::Node skill = item_skill("stateful_weapon_rifle");
        skill["when"]["on_spawn"] = true;
        root["ai"]["skills"].push_back(skill);
    }));
    require(!loads_after("unknown state", kGingerbread, [&](YAML::Node& root) {
        YAML::Node skill = item_skill("fungible_hover_potion");
        skill["when"]["on_state_enter"] = "panicking";
        root["ai"]["skills"].push_back(skill);
    }));
    require(!loads_after("unknown status", kGingerbread, [&](YAML::Node& root) {
        YAML::Node skill = item_skill("fungible_hover_potion");
        skill["when"]["state"] = "chasing";
        skill["when"]["missing_status"] = "invisibility";
        root["ai"]["skills"].push_back(skill);
    }));
    require(!loads_after("hp_below past full", kGingerbread, [&](YAML::Node& root) {
        YAML::Node skill = item_skill("fungible_hover_potion");
        skill["when"]["hp_below"] = 1.5;
        skill["once"] = true;
        root["ai"]["skills"].push_back(skill);
    }));
    require(!loads_after("once on a weapon", kGingerbread, [](YAML::Node& root) {
        root["ai"]["skills"][0]["once"] = true;
    }));
    require(!loads_after("leap swinging another weapon", kLeap, [](YAML::Node& root) {
        root["on_landing"]["weapon"] = 11;
    }));
    // A leap's swing commits on landing only at commit_offset_ticks + 1 (the
    // claw's offset is 8).
    require(!loads_after("leap lead_ticks off by one", kLeap, [](YAML::Node& root) {
        root["on_landing"]["lead_ticks"] = 8;
    }));
    // The gingerbread sees 26 m.
    require(!loads_after("leap band past vision", kGingerbread, [](YAML::Node& root) {
        root["ai"]["skills"][1]["when"]["range_meters"]["max"] = 30.0;
    }));
    require(!loads_after("weapon band past vision", kGingerbread, [](YAML::Node& root) {
        YAML::Node far;
        far["use"]["type"] = "weapon";
        far["use"]["value"] = 10;
        far["when"]["range_meters"]["min"] = 27.0;
        far["when"]["range_meters"]["max"] = 40.0;
        root["ai"]["skills"].push_back(far);
    }));
    require(!loads_after("sentry weapon range", kSentry, [](YAML::Node& root) {
        root["ai"]["skills"][0]["when"]["range_meters"]["max"] = 10.0;
    }));
    require(!loads_after("sentry sp_attack", kSentry, [](YAML::Node& root) {
        YAML::Node leap;
        leap["use"]["type"] = "sp_attack";
        leap["use"]["value"] = "gingerbread_leap";
        leap["when"]["range_meters"]["min"] = 10.0;
        leap["when"]["range_meters"]["max"] = 20.0;
        root["ai"]["skills"].push_back(leap);
    }));
}

}  // namespace

int main() {
    the_catalog_resolves(load(catalog_root()));
    bad_skills_are_refused();
    std::printf("ai_skills_test passed\n");
    return 0;
}
