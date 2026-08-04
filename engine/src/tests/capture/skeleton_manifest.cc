#include "capture/skeleton_manifest.h"

#include <cstdlib>
#include <exception>
#include <fstream>

#include <nlohmann/json.hpp>

namespace network_example::capture {

bool load_skeleton_manifest(
    const std::string& path,
    SkeletonManifest* out_manifest,
    std::string* out_error) {
    if (out_manifest == nullptr) {
        return false;
    }
    const auto fail = [out_error](std::string message) {
        if (out_error != nullptr) {
            *out_error = std::move(message);
        }
        return false;
    };

    std::ifstream file(path);
    if (!file) {
        return fail("failed to open manifest " + path);
    }
    nlohmann::json document;
    try {
        file >> document;
    } catch (const std::exception& error) {
        return fail("manifest parse error in " + path + ": " + error.what());
    }

    out_manifest->asset_id = document.value("asset_id", 0u);
    out_manifest->bone_count = document.value("bone_count", 0u);
    const std::string hash_hex =
        document.value("content_hash", std::string("0x0"));
    out_manifest->content_hash = std::strtoull(hash_hex.c_str(), nullptr, 0);

    if (!document.contains("bones")) {
        return fail("manifest " + path + " has no bones array");
    }
    const auto& bones = document.at("bones");
    out_manifest->nodes.assign(bones.size(), HierarchyNode{});
    for (const auto& bone : bones) {
        const std::size_t index = bone.at("index").get<std::size_t>();
        if (index >= out_manifest->nodes.size()) {
            return fail("manifest " + path + " has an out-of-range bone index");
        }
        out_manifest->nodes[index].name = bone.at("name").get<std::string>();
        out_manifest->nodes[index].parent_index =
            bone.at("parent_index").get<int>();
    }
    if (out_manifest->bone_count != out_manifest->nodes.size()) {
        return fail("manifest " + path + " bone_count disagrees with bones[]");
    }
    return true;
}

}  // namespace network_example::capture
