#include "game_server/src/loadout_director.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "game_server/public/game_server_types.h"

namespace network_example::game_server {

namespace {

// How far past the camp's interaction range a pick is still taken: the player
// may have stepped back while choosing.
constexpr float kPickRangeSlackMeters = 1.0f;

void write_u8(std::vector<std::uint8_t>* out, std::uint8_t value) {
    out->push_back(value);
}

void write_u16(std::vector<std::uint8_t>* out, std::uint16_t value) {
    out->push_back(static_cast<std::uint8_t>(value & 0xffu));
    out->push_back(static_cast<std::uint8_t>(value >> 8));
}

void write_u32(std::vector<std::uint8_t>* out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
}

std::uint32_t read_u32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void write_entries(
    std::vector<std::uint8_t>* out,
    const std::vector<InventorySlotConfig>& entries) {
    write_u8(out, static_cast<std::uint8_t>(entries.size()));
    for (const InventorySlotConfig& entry : entries) {
        write_u32(out, entry.item_template_id);
        write_u16(out, static_cast<std::uint16_t>(entry.quantity));
    }
}

bool entity_state(
    KernelHandle* kernel,
    std::uint32_t net_id,
    KernelServerEntityState* out_state) {
    *out_state = KernelServerEntityState{};
    out_state->struct_size = sizeof(KernelServerEntityState);
    return net_id != 0u && Kernel_ServerGetEntityState(kernel, net_id, out_state);
}

}  // namespace

LoadoutDirector::LoadoutDirector(
    KernelHandle* kernel,
    const GameServerGameplayConfig& config,
    ApplyLoadout apply)
    : kernel_(kernel), config_(config), apply_(std::move(apply)) {}

void LoadoutDirector::handle_event(const KernelEvent& event) {
    if (event.type == KernelEventType_PlayerLeft) {
        // A reconnect is a new arrival here (no identity survives the
        // handshake), so it picks again (D6).
        loadouts_.erase(event.net_id);
        return;
    }
    if (event.type == KernelEventType_UiOpened &&
        camp_template(event.net_id) != nullptr) {
        send_offers(event.peer_id, event.related_net_id, event.net_id);
    }
}

void LoadoutDirector::handle_message(const KernelGameMessage& message) {
    if (message.message_type != GAME_SERVER_MESSAGE_LOADOUT_SELECT) {
        return;
    }
    if (message.payload_size < 5u) {
        reply(message.peer, 0u, GAME_SERVER_LOADOUT_RESULT_MALFORMED, 0u);
        return;
    }
    const std::uint32_t camp = read_u32(message.payload);
    const std::uint32_t pick_count = message.payload[4];
    if (message.payload_size != 5u + pick_count) {
        reply(message.peer, camp, GAME_SERVER_LOADOUT_RESULT_MALFORMED, 0u);
        return;
    }
    const ActorTemplateConfig* camp_config = camp_template(camp);
    if (camp_config == nullptr) {
        reply(message.peer, camp, GAME_SERVER_LOADOUT_RESULT_NOT_A_CAMP, 0u);
        return;
    }
    KernelServerEntityState player_state{};
    KernelServerEntityState camp_state{};
    if (!entity_state(kernel_, message.player_net_id, &player_state) ||
        player_state.hp == 0u) {
        reply(message.peer, camp, GAME_SERVER_LOADOUT_RESULT_DEAD, 0u);
        return;
    }
    if (!entity_state(kernel_, camp, &camp_state)) {
        reply(message.peer, camp, GAME_SERVER_LOADOUT_RESULT_NOT_A_CAMP, 0u);
        return;
    }
    const float dx = player_state.position.x - camp_state.position.x;
    const float dy = player_state.position.y - camp_state.position.y;
    const float dz = player_state.position.z - camp_state.position.z;
    const float reach =
        camp_config->prop.interaction.interaction_range + kPickRangeSlackMeters;
    if (dx * dx + dy * dy + dz * dz > reach * reach) {
        reply(message.peer, camp, GAME_SERVER_LOADOUT_RESULT_OUT_OF_RANGE, 0u);
        return;
    }
    const ActorTemplateConfig* player_template =
        find_actor_template(config_, config_.player.actor_template_id);
    if (player_template == nullptr ||
        pick_count > player_template->inventory_slot_capacity) {
        reply(message.peer, camp, GAME_SERVER_LOADOUT_RESULT_TOO_MANY_PICKS, 0u);
        return;
    }
    std::vector<InventorySlotConfig> picked;
    picked.reserve(pick_count);
    for (std::uint32_t index = 0; index < pick_count; ++index) {
        const std::uint8_t option = message.payload[5u + index];
        if (option >= camp_config->loadout_options.size()) {
            reply(message.peer, camp, GAME_SERVER_LOADOUT_RESULT_BAD_OPTION, 0u);
            return;
        }
        picked.push_back(camp_config->loadout_options[option]);
    }
    // No picks: back to the player template's default.
    if (picked.empty()) {
        loadouts_.erase(message.player_net_id);
    } else {
        loadouts_[message.player_net_id] = std::move(picked);
    }
    if (!apply_ || !apply_(message.player_net_id)) {
        reply(message.peer, camp, GAME_SERVER_LOADOUT_RESULT_APPLY_FAILED, 0u);
        return;
    }
    reply(message.peer, camp, GAME_SERVER_LOADOUT_RESULT_APPLIED,
          static_cast<std::uint8_t>(pick_count));
}

const std::vector<InventorySlotConfig>* LoadoutDirector::loadout_of(
    std::uint32_t player) const {
    const auto found = loadouts_.find(player);
    return found == loadouts_.end() ? nullptr : &found->second;
}

const ActorTemplateConfig* LoadoutDirector::camp_template(
    std::uint32_t camp_net_id) const {
    KernelServerEntityState state{};
    if (kernel_ == nullptr || !entity_state(kernel_, camp_net_id, &state)) {
        return nullptr;
    }
    for (const EntityTemplateConfig& candidate : config_.entity_templates) {
        if (candidate.actor_template_id == state.entity_template_id) {
            return candidate.loadout_options.empty() ? nullptr : &candidate;
        }
    }
    return nullptr;
}

void LoadoutDirector::send_offers(
    std::uint32_t peer,
    std::uint32_t player,
    std::uint32_t camp) {
    const ActorTemplateConfig* camp_config = camp_template(camp);
    const ActorTemplateConfig* player_template =
        find_actor_template(config_, config_.player.actor_template_id);
    if (camp_config == nullptr || player_template == nullptr) {
        return;
    }
    std::vector<std::uint8_t> body;
    write_u32(&body, camp);
    write_u8(&body, static_cast<std::uint8_t>(
        std::min<std::uint32_t>(player_template->inventory_slot_capacity, 255u)));
    write_entries(&body, camp_config->loadout_options);
    const std::vector<InventorySlotConfig>* current = loadout_of(player);
    write_entries(&body, current == nullptr ? std::vector<InventorySlotConfig>{} : *current);
    Kernel_ServerSendGameMessage(
        kernel_,
        peer,
        GAME_SERVER_MESSAGE_LOADOUT_OFFERS,
        body.data(),
        static_cast<std::uint32_t>(body.size()));
}

void LoadoutDirector::reply(
    std::uint32_t peer,
    std::uint32_t camp,
    std::uint8_t result,
    std::uint8_t pick_count) const {
    std::vector<std::uint8_t> body;
    write_u32(&body, camp);
    write_u8(&body, result);
    write_u8(&body, pick_count);
    Kernel_ServerSendGameMessage(
        kernel_,
        peer,
        GAME_SERVER_MESSAGE_LOADOUT_RESULT,
        body.data(),
        static_cast<std::uint32_t>(body.size()));
}

}  // namespace network_example::game_server
