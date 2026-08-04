// Loads the Bazel-generated skeleton manifest JSON (see
// //game_server/skeleton_assets:skeleton_asset_rules.bzl) into the node table
// the hierarchy capture writer expects.

#ifndef ENGINE_SRC_TESTS_CAPTURE_SKELETON_MANIFEST_H_
#define ENGINE_SRC_TESTS_CAPTURE_SKELETON_MANIFEST_H_

#include <cstdint>
#include <string>
#include <vector>

#include "capture/transform_capture.h"

namespace network_example::capture {

struct SkeletonManifest {
    std::uint32_t asset_id = 0;
    std::uint64_t content_hash = 0;
    std::uint32_t bone_count = 0;
    std::vector<HierarchyNode> nodes;  // Indexed by bone index.
};

bool load_skeleton_manifest(
    const std::string& path,
    SkeletonManifest* out_manifest,
    std::string* out_error);

}  // namespace network_example::capture

#endif  // ENGINE_SRC_TESTS_CAPTURE_SKELETON_MANIFEST_H_
