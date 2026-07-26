#ifndef KERNEL_SRC_KERNEL_RPC_JSON_BINDING_H_
#define KERNEL_SRC_KERNEL_RPC_JSON_BINDING_H_

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <type_traits>

#include <nlohmann/json.hpp>

#include "kernel/public/kernel_types.h"

namespace network_example::rpc_json {

using Json = nlohmann::json;

bool has_exact_fields(
    const Json& object,
    std::initializer_list<std::string_view> required);

template <typename T>
std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, bool>
read_json(const Json& value, T* out_value) {
    if (out_value == nullptr ||
        !(value.is_number_integer() || value.is_number_unsigned())) {
        return false;
    }
    try {
        if constexpr (std::is_unsigned_v<T>) {
            std::uint64_t parsed = 0;
            if (value.is_number_unsigned()) {
                parsed = value.get<std::uint64_t>();
            } else {
                const auto signed_value = value.get<std::int64_t>();
                if (signed_value < 0) {
                    return false;
                }
                parsed = static_cast<std::uint64_t>(signed_value);
            }
            if (parsed > std::numeric_limits<T>::max()) {
                return false;
            }
            *out_value = static_cast<T>(parsed);
        } else {
            const auto parsed = value.get<std::int64_t>();
            if (parsed < static_cast<std::int64_t>(
                             std::numeric_limits<T>::min()) ||
                parsed > static_cast<std::int64_t>(
                             std::numeric_limits<T>::max())) {
                return false;
            }
            *out_value = static_cast<T>(parsed);
        }
        return true;
    } catch (const Json::exception&) {
        return false;
    }
}

bool read_json(const Json& value, bool* out_value);
bool read_json(const Json& value, float* out_value);
bool read_json(const Json& value, double* out_value);
bool read_json(const Json& value, KernelVec3* out_value);
bool read_json(const Json& value, KernelQuat* out_value);
bool read_json(const Json& value, KernelServerEntityCreateInfo* out_value);
bool read_json(const Json& value, KernelServerEntityActivateInfo* out_value);

template <typename T>
bool read_param(const Json& object, const char* name, T* out_value) {
    if (!object.is_object()) {
        return false;
    }
    const auto found = object.find(name);
    return found != object.end() && read_json(*found, out_value);
}

Json vec3_json(const KernelVec3& value);
Json quat_json(const KernelQuat& value);
Json entity_state_json(const KernelServerEntityState& state);

}  // namespace network_example::rpc_json

#endif  // KERNEL_SRC_KERNEL_RPC_JSON_BINDING_H_
