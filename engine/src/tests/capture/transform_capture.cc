#include "capture/transform_capture.h"

#include <glm/gtc/matrix_transform.hpp>

namespace network_example::capture {
namespace {

glm::mat4 local_matrix(const LocalTransform& transform) {
    const glm::mat4 translation =
        glm::translate(glm::mat4(1.0f), transform.position);
    const glm::mat4 rotation = glm::mat4_cast(transform.rotation);
    const glm::mat4 scale = glm::scale(glm::mat4(1.0f), transform.scale);
    return translation * rotation * scale;
}

}  // namespace

std::vector<glm::mat4> compute_model_matrices(
    const std::vector<LocalTransform>& locals,
    const std::vector<int>& parents) {
    std::vector<glm::mat4> models(locals.size(), glm::mat4(1.0f));
    for (std::size_t index = 0; index < locals.size(); ++index) {
        const glm::mat4 local = local_matrix(locals[index]);
        const int parent =
            index < parents.size() ? parents[index] : -1;
        models[index] = parent < 0
            ? local
            : models[static_cast<std::size_t>(parent)] * local;
    }
    return models;
}

EntityTransformWriter::EntityTransformWriter(const std::string& path)
    : file_(path) {
    if (!file_) {
        error_ = "failed to open " + path;
        return;
    }
    file_.precision(9);
    file_ << "sample,tick,time_seconds,net_id,template_id,"
             "root_px,root_py,root_pz,"
             "root_qx,root_qy,root_qz,root_qw,"
             "velocity_x,velocity_y,velocity_z\n";
    ok_ = true;
}

void EntityTransformWriter::write(const EntitySample& sample) {
    if (!ok_) {
        return;
    }
    file_ << sample.sample << ',' << sample.tick << ',' << sample.time_seconds
          << ',' << sample.net_id << ',' << sample.template_id << ','
          << sample.position.x << ',' << sample.position.y << ','
          << sample.position.z << ',' << sample.rotation.x << ','
          << sample.rotation.y << ',' << sample.rotation.z << ','
          << sample.rotation.w << ',' << sample.velocity.x << ','
          << sample.velocity.y << ',' << sample.velocity.z << '\n';
}

HierarchyTransformWriter::HierarchyTransformWriter(
    const std::string& path,
    std::vector<HierarchyNode> nodes,
    HierarchyColumns columns)
    : file_(path), nodes_(std::move(nodes)) {
    parents_.reserve(nodes_.size());
    for (const HierarchyNode& node : nodes_) {
        parents_.push_back(node.parent_index);
    }
    if (!file_) {
        error_ = "failed to open " + path;
        return;
    }
    file_.precision(9);
    file_ << "sample,tick,time_seconds,net_id," << columns.index << ','
          << columns.name
          << ",parent_index,"
             "local_px,local_py,local_pz,"
             "local_qx,local_qy,local_qz,local_qw,"
             "local_sx,local_sy,local_sz,"
             "world_px,world_py,world_pz\n";
    ok_ = true;
}

void HierarchyTransformWriter::write_sample(
    std::uint32_t sample,
    std::uint32_t tick,
    double time_seconds,
    std::uint32_t series_id,
    const std::vector<LocalTransform>& locals,
    const glm::vec3& origin_position,
    const glm::quat& origin_rotation) {
    if (!ok_ || locals.size() != nodes_.size()) {
        return;
    }
    const std::vector<glm::mat4> models =
        compute_model_matrices(locals, parents_);
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        const LocalTransform& local = locals[index];
        const glm::vec3 model_position(models[index][3]);
        const glm::vec3 world =
            origin_position + origin_rotation * model_position;
        file_ << sample << ',' << tick << ',' << time_seconds << ','
              << series_id << ',' << index << ',' << nodes_[index].name << ','
              << nodes_[index].parent_index << ',' << local.position.x << ','
              << local.position.y << ',' << local.position.z << ','
              << local.rotation.x << ',' << local.rotation.y << ','
              << local.rotation.z << ',' << local.rotation.w << ','
              << local.scale.x << ',' << local.scale.y << ',' << local.scale.z
              << ',' << world.x << ',' << world.y << ',' << world.z << '\n';
    }
}

}  // namespace network_example::capture
