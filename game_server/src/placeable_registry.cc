#include "game_server/src/placeable_registry.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <optional>
#include <vector>

namespace network_example::game_server {

namespace {

struct Bounds {
    KernelVec3 center{};
    KernelVec3 half_extents{};
};

// The box a collider template fills, in the entity's own frame.
std::optional<Bounds> collider_bounds(const KernelColliderTemplateDefinition& d) {
    const KernelVec4& p = d.shape_params;
    Bounds b;
    b.center = d.center;
    switch (d.shape_type) {
        case KernelColliderShapeType_Aabb:
        case KernelColliderShapeType_OrientedBox:
            b.half_extents = KernelVec3{p.x, p.y, p.z};
            return b;
        case KernelColliderShapeType_Sphere:
            b.half_extents = KernelVec3{p.x, p.x, p.x};
            return b;
        case KernelColliderShapeType_Capsule:  // half_height is the cylinder's
            b.half_extents = KernelVec3{p.y, p.x + p.y, p.y};
            return b;
        case KernelColliderShapeType_Cylinder:
            b.half_extents = KernelVec3{p.y, p.x, p.y};
            return b;
        default:
            return std::nullopt;  // segments and cones size nothing to stand on
    }
}

std::string number(float value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.4f", static_cast<double>(value));
    return text;
}

std::string vec3(const KernelVec3& v) {
    return "[" + number(v.x) + ", " + number(v.y) + ", " + number(v.z) + "]";
}

std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (const char ch : text) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (static_cast<unsigned char>(ch) < 0x20u) {
            char escaped[8];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(ch));
            out += escaped;
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

std::string hex64(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "%016" PRIx64, value);
    return text;
}

}  // namespace

std::uint64_t bundle_bytes_hash(std::span<const std::uint8_t> bundle) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const std::uint8_t byte : bundle) {
        hash ^= byte;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

std::string placeable_registry_json(
    const GameServerGameplayConfig& config,
    std::uint64_t bundle_hash) {
    std::vector<const EntityTemplateConfig*> props;
    for (const EntityTemplateConfig& t : config.entity_templates) {
        if (t.entity_type == KernelEntityType_Prop) props.push_back(&t);
    }
    std::sort(props.begin(), props.end(), [](const EntityTemplateConfig* a, const EntityTemplateConfig* b) {
        return a->name < b->name;
    });

    // 64-bit values as hex strings: a JSON number past 2^53 is not exact in
    // every reader (C#'s included, through some parsers).
    std::string out = "{\n";
    out += "  \"format\": \"placeable_registry/1\",\n";
    out += "  \"catalog_version\": " + std::to_string(config.weapons.catalog_version) + ",\n";
    out += "  \"catalog_hash\": " + quoted(hex64(config.weapons.catalog_hash)) + ",\n";
    out += "  \"bundle_hash\": " + quoted(hex64(bundle_hash)) + ",\n";
    out += "  \"entities\": [";
    for (std::size_t i = 0; i < props.size(); ++i) {
        const EntityTemplateConfig& t = *props[i];
        out += i == 0u ? "\n" : ",\n";
        out += "    {\"name\": " + quoted(t.name) + ", \"id\": " + std::to_string(t.actor_template_id) +
            ", \"entity_type\": \"prop\", \"server_only\": " + (t.server_only ? "true" : "false") +
            ", \"default_grounded\": true";
        std::optional<Bounds> bounds;
        for (const ColliderTemplateConfig& c : config.colliders.templates) {
            if (t.collider_template_id != 0u && c.definition.template_id == t.collider_template_id) {
                bounds = collider_bounds(c.definition);
                break;
            }
        }
        if (bounds) {
            out += ", \"bounds\": {\"center\": " + vec3(bounds->center) +
                ", \"half_extents\": " + vec3(bounds->half_extents) + "}";
        } else {
            out += ", \"bounds\": null";
        }
        out += "}";
    }
    out += props.empty() ? "]\n" : "\n  ]\n";
    out += "}\n";
    return out;
}

}  // namespace network_example::game_server
