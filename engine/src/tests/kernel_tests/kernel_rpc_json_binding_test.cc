#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "kernel/public/kernel_types.h"
#include "kernel/src/kernel_rpc_json_binding.h"

namespace {

using Json = nlohmann::json;

void reads_create_info_by_type() {
    const Json value = {
        {"entity_type", 1},
        {"actor_type", 2},
        {"owner_peer", 3},
        {"position", {{"x", 4.0}, {"y", 5.0}, {"z", 6.0}}},
        {"rotation", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}, {"w", 1.0}}},
        {"animation_state", 7},
        {"visual_flags", 8},
        {"actor_template_id", 9},
    };

    KernelServerEntityCreateInfo info{};
    assert(network_example::rpc_json::read_json(value, &info));
    assert(info.struct_size == sizeof(info));
    assert(info.entity_type == 1);
    assert(info.actor_type == 2);
    assert(info.owner_peer == 3);
    assert(info.position.x == 4.0f);
    assert(info.position.y == 5.0f);
    assert(info.position.z == 6.0f);
    assert(info.rotation.w == 1.0f);
    assert(info.animation_state == 7);
    assert(info.visual_flags == 8);
    assert(info.actor_template_id == 9);
}

void reads_create_info_entity_template_id_when_present() {
    const Json value = {
        {"entity_type", 0},
        {"actor_type", 0},
        {"owner_peer", 0},
        {"position", {{"x", 4.0}, {"y", 5.0}, {"z", 6.0}}},
        {"rotation", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}, {"w", 1.0}}},
        {"animation_state", 0},
        {"visual_flags", 0},
        {"actor_template_id", 0},
        {"entity_template_id", 100},
    };

    KernelServerEntityCreateInfo info{};
    assert(network_example::rpc_json::read_json(value, &info));
    assert(info.struct_size == sizeof(info));
    assert(info.entity_template_id == 100);
}

void rejects_shape_and_range_errors() {
    KernelServerEntityCreateInfo info{};

    Json missing_field = {
        {"entity_type", 1},
        {"actor_type", 2},
        {"owner_peer", 3},
        {"position", {{"x", 4.0}, {"y", 5.0}, {"z", 6.0}}},
        {"rotation", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}, {"w", 1.0}}},
        {"animation_state", 7},
        {"visual_flags", 8},
    };
    assert(!network_example::rpc_json::read_json(missing_field, &info));

    Json extra_field = missing_field;
    extra_field["actor_template_id"] = 9;
    extra_field["unexpected"] = 10;
    assert(!network_example::rpc_json::read_json(extra_field, &info));

    Json overflow = missing_field;
    overflow["actor_template_id"] = 9;
    overflow["animation_state"] = 70000;
    assert(!network_example::rpc_json::read_json(overflow, &info));
}

}  // namespace

int main() {
    reads_create_info_by_type();
    reads_create_info_entity_template_id_when_present();
    rejects_shape_and_range_errors();
    return 0;
}
