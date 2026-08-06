// Generic transform capture: record hierarchical or flat transform series to CSV.
//
// The locomotion capture records a skeleton (41 bones, parented) plus the root
// entity transform, but nothing here is skeleton specific. A capture of, say,
// the movement paths of N entities is the same thing with one flat node per
// entity, so both use the same writers and the same CSV schema:
//
//   EntityTransformWriter    -> one row per (sample, entity): world TRS + velocity
//   HierarchyTransformWriter -> one row per (sample, node): local TRS + FK world
//
// The hierarchy writer's index/name columns are configurable so a non-skeleton
// capture reads naturally (`entity_index`/`entity_name` instead of
// `bone_index`/`bone_name`); tools/capture/render_transforms.py detects the
// column names, so a new capture needs no renderer change.

#ifndef ENGINE_SRC_TESTS_CAPTURE_TRANSFORM_CAPTURE_H_
#define ENGINE_SRC_TESTS_CAPTURE_TRANSFORM_CAPTURE_H_

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace network_example::capture {

struct LocalTransform {
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // (w, x, y, z)
    glm::vec3 scale{1.0f, 1.0f, 1.0f};
};

struct HierarchyNode {
    std::string name;
    int parent_index = -1;  // < 0 for a root node.
};

// Column names for the per-node identity columns.
struct HierarchyColumns {
    std::string index = "bone_index";
    std::string name = "bone_name";
};

// Composes local transforms into model space. `locals` must be ordered so that
// a parent always precedes its children (the ozz runtime order).
std::vector<glm::mat4> compute_model_matrices(
    const std::vector<LocalTransform>& locals,
    const std::vector<int>& parents);

struct EntitySample {
    std::uint32_t sample = 0;
    std::uint32_t tick = 0;
    double time_seconds = 0.0;
    std::uint32_t net_id = 0;
    std::uint32_t template_id = 0;
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f, 0.0f, 0.0f};
};

// Flat per-entity world transform series. Multiple entities per sample are
// allowed: that is how an entity path capture records more than one subject.
class EntityTransformWriter {
  public:
    explicit EntityTransformWriter(const std::string& path);

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    void write(const EntitySample& sample);

  private:
    std::ofstream file_;
    bool ok_ = false;
    std::string error_;
};

// Hierarchical local-transform series with forward-kinematics world positions.
class HierarchyTransformWriter {
  public:
    HierarchyTransformWriter(
        const std::string& path,
        std::vector<HierarchyNode> nodes,
        HierarchyColumns columns = HierarchyColumns{});

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }
    std::size_t node_count() const { return nodes_.size(); }

    // `locals` must hold exactly node_count() entries in parent-before-child
    // order. `origin_*` is the world transform the hierarchy hangs off (the
    // entity root); pass identity for a hierarchy already in world space.
    void write_sample(
        std::uint32_t sample,
        std::uint32_t tick,
        double time_seconds,
        std::uint32_t series_id,
        const std::vector<LocalTransform>& locals,
        const glm::vec3& origin_position,
        const glm::quat& origin_rotation);

  private:
    std::ofstream file_;
    std::vector<HierarchyNode> nodes_;
    std::vector<int> parents_;
    bool ok_ = false;
    std::string error_;
};

}  // namespace network_example::capture

#endif  // ENGINE_SRC_TESTS_CAPTURE_TRANSFORM_CAPTURE_H_
