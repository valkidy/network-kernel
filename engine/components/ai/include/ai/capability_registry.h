#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_CAPABILITY_REGISTRY_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_CAPABILITY_REGISTRY_H_

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace network_example::ai {

struct ScenarioRequirements {
    std::vector<std::string> required_features;
    std::vector<std::string> required_nodes;
    std::vector<std::string> required_scores;
    std::vector<std::string> required_queries;
};

struct CapabilityReport {
    std::vector<std::string> missing_features;
    std::vector<std::string> missing_nodes;
    std::vector<std::string> missing_scores;
    std::vector<std::string> missing_queries;
    std::vector<std::string> suggested_extensions;

    bool supported() const;
};

class CapabilityRegistry {
public:
    void add_feature(std::string name);
    void add_node_type(std::string name);
    void add_score_function(std::string name);
    void add_query(std::string name);

    bool has_feature(std::string_view name) const;
    bool has_node_type(std::string_view name) const;
    bool has_score_function(std::string_view name) const;
    bool has_query(std::string_view name) const;

    CapabilityReport validate(const ScenarioRequirements& requirements) const;

private:
    std::unordered_set<std::string> features_;
    std::unordered_set<std::string> node_types_;
    std::unordered_set<std::string> score_functions_;
    std::unordered_set<std::string> queries_;
};

CapabilityRegistry make_default_capability_registry();

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_CAPABILITY_REGISTRY_H_
