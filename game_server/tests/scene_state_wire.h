// SCENE_STATE as the scene tests read it (game_server_types.h,
// GAME_SERVER_ABI_VERSION 6): the header, the two gates, then the slots.
#ifndef GAME_SERVER_TESTS_SCENE_STATE_WIRE_H_
#define GAME_SERVER_TESTS_SCENE_STATE_WIRE_H_

#include <cstdint>
#include <cstring>

#include "game_server/public/game_server_types.h"
#include "kernel/public/kernel_types.h"

namespace scene_state_wire {

constexpr std::uint32_t kHeaderBytes = 15u;
constexpr std::uint32_t kGateBytes = 53u;
// Where slot_count sits; the slots follow it.
constexpr std::uint32_t kSlotCountAt = kHeaderBytes + 2u * kGateBytes;
constexpr std::uint32_t kSlotsAt = kSlotCountAt + 1u;

struct Gate {
    std::uint8_t state = GAME_SERVER_SCENE_GATE_NONE;
    std::uint32_t entrance_net_id = 0;
    KernelVec3 entrance{};
    KernelVec3 entrance_forward{};
    KernelVec3 exit{};
    KernelVec3 exit_forward{};
};

struct Header {
    std::uint8_t flow = 0;
    std::uint8_t level_slot = 0;
    std::uint32_t generation = 0;
    std::uint8_t outcome = 0;
    std::uint32_t owner = 0;
    std::uint16_t defeated = 0;
    std::uint16_t target = 0;
    Gate level_gate;
    Gate return_gate;
    std::uint8_t slot_count = 0;
};

inline std::uint32_t u32_at(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
        (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

inline float f32_at(const std::uint8_t* p) {
    const std::uint32_t bits = u32_at(p);
    float v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

inline KernelVec3 vec3_at(const std::uint8_t* p) {
    return KernelVec3{f32_at(p), f32_at(p + 4), f32_at(p + 8)};
}

inline Gate gate_at(const std::uint8_t* p) {
    Gate g;
    g.state = p[0];
    g.entrance_net_id = u32_at(p + 1);
    g.entrance = vec3_at(p + 5);
    g.entrance_forward = vec3_at(p + 17);
    g.exit = vec3_at(p + 29);
    g.exit_forward = vec3_at(p + 41);
    return g;
}

// False when the body is too short for the fixed part.
inline bool read_header(const std::uint8_t* p, std::uint32_t size, Header* out) {
    if (p == nullptr || size < kSlotsAt) return false;
    out->flow = p[0];
    out->level_slot = p[1];
    out->generation = u32_at(p + 2);
    out->outcome = p[6];
    out->owner = u32_at(p + 7);
    out->defeated = static_cast<std::uint16_t>(p[11] | (p[12] << 8));
    out->target = static_cast<std::uint16_t>(p[13] | (p[14] << 8));
    out->level_gate = gate_at(p + kHeaderBytes);
    out->return_gate = gate_at(p + kHeaderBytes + kGateBytes);
    out->slot_count = p[kSlotCountAt];
    return true;
}

// The gate a player can walk through now: the level gate, else the return
// gate; null when neither is open.
inline const Gate* open_gate(const Header& h) {
    if (h.level_gate.state == GAME_SERVER_SCENE_GATE_OPEN) return &h.level_gate;
    if (h.return_gate.state == GAME_SERVER_SCENE_GATE_OPEN) return &h.return_gate;
    return nullptr;
}

// Where to stand to be in a gate's trigger: 0.3 m in front of the entrance,
// so a capsule (radius 0.35) reaches 5 cm into the gate, which reaches 0.1 m
// out from the frame. Standing there is walking in.
inline KernelVec3 step_in(const Gate& g) {
    return KernelVec3{
        g.entrance.x - g.entrance_forward.x * 0.3f,
        g.entrance.y,
        g.entrance.z - g.entrance_forward.z * 0.3f};
}

}  // namespace scene_state_wire

#endif  // GAME_SERVER_TESTS_SCENE_STATE_WIRE_H_
