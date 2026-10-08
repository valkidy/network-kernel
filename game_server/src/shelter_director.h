#ifndef GAME_SERVER_SRC_SHELTER_DIRECTOR_H_
#define GAME_SERVER_SRC_SHELTER_DIRECTOR_H_

#include <cstdint>
#include <map>
#include <vector>

#include "kernel/public/kernel_api.h"

namespace network_example::game_server {

// Who goes into which building, and when they come out. Every building shares
// one rule (D8), so this never looks at a ui_id: which interface opens is the
// client's business. Activating a building from outside asks to go in;
// activating the one you are in asks to come out.
//
// It decides and the kernel carries out: requests go through the command queue
// (Kernel_ServerEnqueueEntityShelter), and the kernel re-checks every rule when
// it runs them a tick later. So what is recorded here is never what was asked
// for, only what the kernel reported doing (KernelEventType_ShelterChanged).
// A request the kernel refused simply leaves nothing to record.
class ShelterDirector {
public:
    explicit ShelterDirector(KernelHandle* kernel);

    void handle_event(const KernelEvent& event);

    // The building `actor` is inside, or 0.
    std::uint32_t shelter_of(std::uint32_t actor) const;
    // Everyone inside `building`, in net id order.
    std::vector<std::uint32_t> occupants_of(std::uint32_t building) const;
    // Where `actor` stood when it was sent inside: outside the building, where
    // it comes back out. False when it is not inside one. Inside, its own
    // position is the building's, so anything set down "at its feet" goes
    // here instead, as the kernel's own drops and a camp's Transfer do.
    bool entry_position_of(std::uint32_t actor, KernelVec3* out_position) const;

private:
    void request(std::uint32_t actor, std::uint32_t building) const;

    KernelHandle* kernel_ = nullptr;
    // Occupant -> building, as the kernel last reported it.
    std::map<std::uint32_t, std::uint32_t> shelter_of_;
    // Actor -> where it stood when it asked to go in.
    std::map<std::uint32_t, KernelVec3> entry_position_;
};

}  // namespace network_example::game_server

#endif  // GAME_SERVER_SRC_SHELTER_DIRECTOR_H_
