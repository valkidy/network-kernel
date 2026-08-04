#include "capture/path_script.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using network_example::capture::PathScript;
using network_example::capture::PathStep;

int failure_count = 0;

void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        ++failure_count;
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

bool near(float lhs, float rhs) {
    return std::fabs(lhs - rhs) < 1e-4f;
}

void test_axis_alias() {
    PathScript script;
    std::string error;
    REQUIRE(PathScript::parse("+X", 30.0f, &script, &error));
    glm::vec3 position(0.0f, 0.0f, 0.0f);
    for (int tick = 0; tick < 100; ++tick) {
        const glm::vec2 move = script.move_input(position);
        REQUIRE(near(move.x, 1.0f) && near(move.y, 0.0f));
        position.x += 0.1f;
    }
    REQUIRE(!script.finished());

    PathScript negative_z;
    REQUIRE(PathScript::parse("-Z", 30.0f, &negative_z, &error));
    const glm::vec2 move = negative_z.move_input(glm::vec3(0.0f));
    REQUIRE(near(move.x, 0.0f) && near(move.y, -1.0f));
}

void test_forward_then_turn() {
    PathScript script;
    std::string error;
    REQUIRE(PathScript::parse(
        "forward 10m; turn 90; forward 5m", 30.0f, &script, &error));

    glm::vec3 position(0.0f, 0.0f, 0.0f);
    // First step: +X until 10 m of travel.
    for (int tick = 0; tick < 20; ++tick) {
        const glm::vec2 move = script.move_input(position);
        REQUIRE(near(move.x, 1.0f) && near(move.y, 0.0f));
        position.x += 0.5f;
    }
    // 10 m reached: the turn applies and the second forward starts along +Z.
    glm::vec2 move = script.move_input(position);
    REQUIRE(near(move.x, 0.0f) && near(move.y, 1.0f));
    for (int tick = 0; tick < 9; ++tick) {
        position.z += 0.5f;
        move = script.move_input(position);
        REQUIRE(near(move.x, 0.0f) && near(move.y, 1.0f));
    }
    // 5 m of lateral travel completes the script; the subject stands still.
    position.z += 0.5f;
    move = script.move_input(position);
    REQUIRE(script.finished());
    REQUIRE(near(move.x, 0.0f) && near(move.y, 0.0f));
}

void test_wait_holds_for_whole_seconds() {
    PathScript script;
    std::string error;
    REQUIRE(PathScript::parse("wait 2s; forward 1m", 30.0f, &script, &error));
    const glm::vec3 position(0.0f, 0.0f, 0.0f);
    for (int tick = 0; tick < 60; ++tick) {
        const glm::vec2 move = script.move_input(position);
        REQUIRE(near(move.x, 0.0f) && near(move.y, 0.0f));
    }
    const glm::vec2 move = script.move_input(position);
    REQUIRE(near(move.x, 1.0f) && near(move.y, 0.0f));
}

void test_chinese_keywords_and_compact_spelling() {
    PathScript script;
    std::string error;
    REQUIRE(PathScript::parse("前進10m; 旋轉45度", 30.0f, &script, &error));
    REQUIRE(script.steps().size() == 2);
    REQUIRE(script.steps()[0].kind == PathStep::Kind::kForward);
    REQUIRE(near(script.steps()[0].value, 10.0f));
    REQUIRE(script.steps()[1].kind == PathStep::Kind::kTurn);
    REQUIRE(near(script.steps()[1].value, 45.0f));
}

void test_rejects_bad_input() {
    PathScript script;
    std::string error;
    REQUIRE(!PathScript::parse("", 30.0f, &script, &error));
    REQUIRE(!PathScript::parse("sideways 3m", 30.0f, &script, &error));
    REQUIRE(!PathScript::parse("forward", 30.0f, &script, &error));
    REQUIRE(!PathScript::parse("forward -3m", 30.0f, &script, &error));
    REQUIRE(!error.empty());
}

}  // namespace

int main() {
    test_axis_alias();
    test_forward_then_turn();
    test_wait_holds_for_whole_seconds();
    test_chinese_keywords_and_compact_spelling();
    test_rejects_bad_input();
    if (failure_count != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failure_count);
        return 1;
    }
    std::printf("path_script_test passed\n");
    return 0;
}
