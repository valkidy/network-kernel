#ifndef ENGINE_COMPONENTS_AI_INCLUDE_AI_SCENARIO_ANALYSIS_H_
#define ENGINE_COMPONENTS_AI_INCLUDE_AI_SCENARIO_ANALYSIS_H_

#include <string>
#include <vector>

#include "ai/capability_registry.h"

namespace network_example::ai {

struct ScenarioAnalysisResult {
    std::string actor;
    std::vector<std::string> features;
    std::vector<std::string> actions;
    std::vector<std::string> rules;
    ScenarioRequirements requirements;
};

enum class YamlGenerationStatus {
    kSuccess,
    kUnsupported,
};

struct YamlGenerationResult {
    YamlGenerationStatus status = YamlGenerationStatus::kUnsupported;
    std::string yaml;
    std::vector<std::string> required_nodes;
    std::vector<std::string> required_features;
    CapabilityReport report;

    bool success() const { return status == YamlGenerationStatus::kSuccess; }
};

}  // namespace network_example::ai

#endif  // ENGINE_COMPONENTS_AI_INCLUDE_AI_SCENARIO_ANALYSIS_H_
