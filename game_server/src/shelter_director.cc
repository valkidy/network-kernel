#include "game_server/src/shelter_director.h"

#include "kernel/src/kernel_api_internal.h"

namespace network_example::game_server {

ShelterDirector::ShelterDirector(KernelHandle* kernel) : kernel_(kernel) {}

void ShelterDirector::handle_event(const KernelEvent& event) {
    switch (event.type) {
        case KernelEventType_UiOpened: {
            const std::uint32_t building = event.net_id;
            const std::uint32_t actor = event.related_net_id;
            if (building == 0u || actor == 0u) {
                return;
            }
            const std::uint32_t inside = shelter_of(actor);
            if (inside == building) {
                request(actor, 0u);
            } else if (inside == 0u) {
                // Recorded now: once inside, the actor stands at the building.
                KernelServerEntityState state{};
                state.struct_size = sizeof(state);
                if (kernel_ != nullptr &&
                    Kernel_ServerGetEntityState(kernel_, actor, &state)) {
                    entry_position_[actor] = state.position;
                }
                request(actor, building);
            }
            // Inside another building: the kernel only lets an occupant
            // activate its own, so this cannot happen. Nothing to do if it did.
            return;
        }
        case KernelEventType_ShelterChanged:
            if (event.code != 0u) {
                shelter_of_[event.net_id] = event.code;
            } else {
                shelter_of_.erase(event.net_id);
                entry_position_.erase(event.net_id);
            }
            return;
        case KernelEventType_EntityDestroyed:
            // An occupant removed outright -- a disconnect destroys the player
            // without a ShelterChanged -- has nothing left to come out of. A
            // building needs nothing here: the kernel lets its occupants out
            // before it goes, and their ShelterChanged arrive first.
            shelter_of_.erase(event.net_id);
            entry_position_.erase(event.net_id);
            return;
        case KernelEventType_PlayerLeft:
            shelter_of_.erase(event.net_id);
            entry_position_.erase(event.net_id);
            return;
        default:
            return;
    }
}

std::uint32_t ShelterDirector::shelter_of(std::uint32_t actor) const {
    const auto found = shelter_of_.find(actor);
    return found == shelter_of_.end() ? 0u : found->second;
}

std::vector<std::uint32_t> ShelterDirector::occupants_of(
    std::uint32_t building) const {
    std::vector<std::uint32_t> occupants;
    for (const auto& [actor, shelter] : shelter_of_) {
        if (shelter == building) {
            occupants.push_back(actor);
        }
    }
    return occupants;
}

bool ShelterDirector::entry_position_of(
    std::uint32_t actor,
    KernelVec3* out_position) const {
    if (shelter_of(actor) == 0u) {
        return false;
    }
    const auto found = entry_position_.find(actor);
    if (found == entry_position_.end()) {
        return false;
    }
    *out_position = found->second;
    return true;
}

void ShelterDirector::request(std::uint32_t actor, std::uint32_t building) const {
    if (kernel_ == nullptr) {
        return;
    }
    Kernel_ServerEnqueueEntityShelter(
        kernel_, KernelCommandSource_Internal, actor, building);
}

}  // namespace network_example::game_server
