#include "ai/ai_context.h"
#include "ai/ai_node.h"
#include "ai/ai_tree.h"
#include "ai/node_factory.h"

#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class ScriptedNode final : public network_example::ai::AINode {
public:
    explicit ScriptedNode(std::vector<network_example::ai::NodeStatus> statuses)
        : statuses_(std::move(statuses)) {}

    network_example::ai::NodeStatus tick(
        const network_example::ai::AIContext&,
        network_example::ai::AICommandBuffer*) override {
        ++tick_count;
        if (next_status_ < statuses_.size()) {
            return statuses_[next_status_++];
        }
        return statuses_.back();
    }

    void halt(const network_example::ai::AIContext&,
              network_example::ai::AICommandBuffer*) override {
        ++halt_count;
    }

    int tick_count = 0;
    int halt_count = 0;

private:
    std::vector<network_example::ai::NodeStatus> statuses_;
    std::size_t next_status_ = 0;
};

std::unique_ptr<ScriptedNode> node(
    std::vector<network_example::ai::NodeStatus> statuses) {
    return std::make_unique<ScriptedNode>(std::move(statuses));
}

}  // namespace

int main() {
    using network_example::ai::NodeStatus;

    network_example::ai::AIContext context;
    network_example::ai::AICommandBuffer commands;

    auto selector_first = node({NodeStatus::kFailure});
    auto selector_second = node({NodeStatus::kRunning, NodeStatus::kSuccess});
    ScriptedNode* selector_first_ptr = selector_first.get();
    ScriptedNode* selector_second_ptr = selector_second.get();

    std::vector<network_example::ai::NodePtr> selector_children;
    selector_children.push_back(std::move(selector_first));
    selector_children.push_back(std::move(selector_second));
    auto selector = network_example::ai::make_selector(std::move(selector_children));

    assert(selector->tick(context, &commands) == NodeStatus::kRunning);
    assert(selector_first_ptr->tick_count == 1);
    assert(selector_second_ptr->tick_count == 1);
    assert(selector->tick(context, &commands) == NodeStatus::kSuccess);
    assert(selector_second_ptr->halt_count == 0);

    auto sequence_first = node({NodeStatus::kSuccess});
    auto sequence_second = node({NodeStatus::kRunning, NodeStatus::kSuccess});
    ScriptedNode* sequence_first_ptr = sequence_first.get();
    ScriptedNode* sequence_second_ptr = sequence_second.get();

    std::vector<network_example::ai::NodePtr> sequence_children;
    sequence_children.push_back(std::move(sequence_first));
    sequence_children.push_back(std::move(sequence_second));
    auto sequence = network_example::ai::make_sequence(std::move(sequence_children));

    assert(sequence->tick(context, &commands) == NodeStatus::kRunning);
    assert(sequence->tick(context, &commands) == NodeStatus::kSuccess);
    assert(sequence_first_ptr->tick_count == 1);
    assert(sequence_second_ptr->tick_count == 2);

    auto low = node({NodeStatus::kSuccess});
    auto high = node({NodeStatus::kRunning});
    ScriptedNode* low_ptr = low.get();
    ScriptedNode* high_ptr = high.get();
    std::vector<network_example::ai::UtilityCandidate> candidates;
    candidates.push_back(network_example::ai::UtilityCandidate{
        "Low",
        [](const network_example::ai::AIContext&) {
            return network_example::ai::ScoreResult::valid_score(0.1f);
        },
        std::move(low),
    });
    candidates.push_back(network_example::ai::UtilityCandidate{
        "High",
        [](const network_example::ai::AIContext&) {
            return network_example::ai::ScoreResult::valid_score(0.9f);
        },
        std::move(high),
    });

    auto utility = network_example::ai::make_utility_selector(std::move(candidates));
    assert(utility->tick(context, &commands) == NodeStatus::kRunning);
    assert(low_ptr->tick_count == 0);
    assert(high_ptr->tick_count == 1);

    auto tie_first = node({NodeStatus::kSuccess});
    auto tie_second = node({NodeStatus::kSuccess});
    ScriptedNode* tie_first_ptr = tie_first.get();
    ScriptedNode* tie_second_ptr = tie_second.get();
    std::vector<network_example::ai::UtilityCandidate> ties;
    ties.push_back(network_example::ai::UtilityCandidate{
        "First",
        [](const network_example::ai::AIContext&) {
            return network_example::ai::ScoreResult::valid_score(0.5f);
        },
        std::move(tie_first),
    });
    ties.push_back(network_example::ai::UtilityCandidate{
        "Second",
        [](const network_example::ai::AIContext&) {
            return network_example::ai::ScoreResult::valid_score(0.5f);
        },
        std::move(tie_second),
    });
    auto tie_utility =
        network_example::ai::make_utility_selector(std::move(ties));
    assert(tie_utility->tick(context, &commands) == NodeStatus::kSuccess);
    assert(tie_first_ptr->tick_count == 1);
    assert(tie_second_ptr->tick_count == 0);

    auto switching_first = node({NodeStatus::kRunning});
    auto switching_second = node({NodeStatus::kRunning});
    ScriptedNode* switching_first_ptr = switching_first.get();
    ScriptedNode* switching_second_ptr = switching_second.get();
    context.set_feature("pickSecond", false);
    std::vector<network_example::ai::UtilityCandidate> switching;
    switching.push_back(network_example::ai::UtilityCandidate{
        "First",
        [](const network_example::ai::AIContext& ctx) {
            return network_example::ai::ScoreResult::valid_score(
                ctx.get_bool("pickSecond").value_or(false) ? 0.1f : 0.9f);
        },
        std::move(switching_first),
    });
    switching.push_back(network_example::ai::UtilityCandidate{
        "Second",
        [](const network_example::ai::AIContext& ctx) {
            return network_example::ai::ScoreResult::valid_score(
                ctx.get_bool("pickSecond").value_or(false) ? 0.9f : 0.1f);
        },
        std::move(switching_second),
    });
    auto switching_utility =
        network_example::ai::make_utility_selector(std::move(switching));
    assert(switching_utility->tick(context, &commands) == NodeStatus::kRunning);
    context.set_feature("pickSecond", true);
    assert(switching_utility->tick(context, &commands) == NodeStatus::kRunning);
    assert(switching_first_ptr->halt_count == 1);
    assert(switching_second_ptr->tick_count == 1);

    return 0;
}
