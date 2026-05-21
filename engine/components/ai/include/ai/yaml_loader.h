#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_YAML_LOADER_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_YAML_LOADER_H_

#include <string>
#include <vector>

#include "ai/ai_tree.h"
#include "ai/capability_registry.h"

namespace network_example::ai {

struct YamlLoadResult {
    NodePtr root;
    std::vector<std::string> errors;
    std::vector<std::string> missing_nodes;
    std::vector<std::string> missing_scores;
    std::vector<std::string> missing_features;
    std::vector<std::string> required_nodes;
    std::vector<std::string> required_scores;
    std::vector<std::string> required_features;

    bool success() const {
        return root != nullptr && errors.empty() && missing_nodes.empty() &&
               missing_scores.empty() && missing_features.empty();
    }
};

YamlLoadResult load_tree_from_yaml(const std::string& yaml);
YamlLoadResult load_tree_from_yaml(
    const std::string& yaml,
    const CapabilityRegistry& registry);
CapabilityReport validate_yaml_capabilities(
    const std::string& yaml,
    const CapabilityRegistry& registry);

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_YAML_LOADER_H_
