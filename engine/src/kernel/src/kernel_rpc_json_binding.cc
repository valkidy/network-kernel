#include "kernel/src/kernel_rpc_json_binding.h"

#include <algorithm>
#include <array>

namespace network_example::rpc_json {

bool has_exact_fields(
    const Json& object,
    std::initializer_list<std::string_view> required) {
    if (!object.is_object() || object.size() != required.size()) {
        return false;
    }
    return std::all_of(
        required.begin(),
        required.end(),
        [&object](std::string_view field) {
            return object.contains(std::string(field));
        });
}

bool read_json(const Json& value, bool* out_value) {
    if (out_value == nullptr || !value.is_boolean()) {
        return false;
    }
    *out_value = value.get<bool>();
    return true;
}

bool read_json(const Json& value, float* out_value) {
    if (out_value == nullptr || !value.is_number()) {
        return false;
    }
    try {
        *out_value = value.get<float>();
        return true;
    } catch (const Json::exception&) {
        return false;
    }
}

bool read_json(const Json& value, double* out_value) {
    if (out_value == nullptr || !value.is_number()) {
        return false;
    }
    try {
        *out_value = value.get<double>();
        return true;
    } catch (const Json::exception&) {
        return false;
    }
}

bool read_json(const Json& value, KernelVec3* out_value) {
    if (out_value == nullptr ||
        !has_exact_fields(value, {"x", "y", "z"})) {
        return false;
    }
    return read_param(value, "x", &out_value->x) &&
           read_param(value, "y", &out_value->y) &&
           read_param(value, "z", &out_value->z);
}

bool read_json(const Json& value, KernelQuat* out_value) {
    if (out_value == nullptr ||
        !has_exact_fields(value, {"x", "y", "z", "w"})) {
        return false;
    }
    return read_param(value, "x", &out_value->x) &&
           read_param(value, "y", &out_value->y) &&
           read_param(value, "z", &out_value->z) &&
           read_param(value, "w", &out_value->w);
}

bool read_json(const Json& value, KernelServerEntityCreateInfo* out_value) {
    constexpr std::array<std::string_view, 8> kRequiredFields = {
        "entity_type",
        "actor_type",
        "owner_peer",
        "position",
        "rotation",
        "animation_state",
        "visual_flags",
        "actor_template_id",
    };
    if (out_value == nullptr || !value.is_object() ||
        (value.size() != kRequiredFields.size() &&
         value.size() != kRequiredFields.size() + 1u)) {
        return false;
    }
    for (std::string_view field : kRequiredFields) {
        if (!value.contains(std::string(field))) {
            return false;
        }
    }
    if (value.size() == kRequiredFields.size() + 1u &&
        !value.contains("entity_template_id")) {
        return false;
    }
    out_value->struct_size = sizeof(*out_value);
    if (!read_param(value, "entity_type", &out_value->entity_type) ||
        !read_param(value, "actor_type", &out_value->actor_type) ||
        !read_param(value, "owner_peer", &out_value->owner_peer) ||
        !read_param(value, "position", &out_value->position) ||
        !read_param(value, "rotation", &out_value->rotation) ||
        !read_param(value, "animation_state", &out_value->animation_state) ||
        !read_param(value, "visual_flags", &out_value->visual_flags) ||
        !read_param(value, "actor_template_id", &out_value->actor_template_id)) {
        return false;
    }
    if (value.contains("entity_template_id") &&
        !read_param(value, "entity_template_id", &out_value->entity_template_id)) {
        return false;
    }
    return true;
}

bool read_json(const Json& value, KernelServerEntityActivateInfo* out_value) {
    if (out_value == nullptr ||
        !has_exact_fields(
            value,
            {
                "subject_net_id",
                "instigator_net_id",
                "target_net_id",
                "action_instance_id",
                "request_id",
            })) {
        return false;
    }
    out_value->struct_size = sizeof(*out_value);
    return read_param(value, "subject_net_id", &out_value->subject_net_id) &&
           read_param(value, "instigator_net_id", &out_value->instigator_net_id) &&
           read_param(value, "target_net_id", &out_value->target_net_id) &&
           read_param(value, "action_instance_id", &out_value->action_instance_id) &&
           read_param(value, "request_id", &out_value->request_id);
}

bool read_json(const Json& value, KernelGameplayRequest* out_value) {
    if (out_value == nullptr ||
        !has_exact_fields(value, {
            "requester_peer",
            "request_id",
            "instigator_net_id",
            "domain_action",
            "selected_item_instance_id",
            "target_net_id",
            "requested_quantity",
            "placement_position",
            "throw_direction",
        })) {
        return false;
    }
    out_value->struct_size = sizeof(*out_value);
    return read_param(value, "requester_peer", &out_value->requester_peer) &&
        read_param(value, "request_id", &out_value->request_id) &&
        read_param(value, "instigator_net_id", &out_value->instigator_net_id) &&
        read_param(value, "domain_action", &out_value->domain_action) &&
        read_param(value, "selected_item_instance_id",
            &out_value->selected_item_instance_id) &&
        read_param(value, "target_net_id", &out_value->target_net_id) &&
        read_param(value, "requested_quantity", &out_value->requested_quantity) &&
        read_param(value, "placement_position", &out_value->placement_position) &&
        read_param(value, "throw_direction", &out_value->throw_direction);
}

Json vec3_json(const KernelVec3& value) {
    return {{"x", value.x}, {"y", value.y}, {"z", value.z}};
}

Json quat_json(const KernelQuat& value) {
    return {
        {"x", value.x},
        {"y", value.y},
        {"z", value.z},
        {"w", value.w},
    };
}

Json entity_state_json(const KernelServerEntityState& state) {
    return {
        {"net_id", state.net_id},
        {"entity_type", state.entity_type},
        {"actor_type", state.actor_type},
        {"owner_peer", state.owner_peer},
        {"position", vec3_json(state.position)},
        {"rotation", quat_json(state.rotation)},
        {"velocity", vec3_json(state.velocity)},
        {"hp", state.hp},
        {"max_hp", state.max_hp},
        {"animation_state", state.animation_state},
        {"visual_flags", state.visual_flags},
        {"valid", state.valid},
        {"actor_template_id", state.actor_template_id},
    };
}

}  // namespace network_example::rpc_json
