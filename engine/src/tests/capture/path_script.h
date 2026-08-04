// Parses a human-written movement description into a per-tick move input.
//
// The capture harness drives its subject the same way a player does: a 2D move
// vector in the world XZ plane (KernelPlayerInput::move, see
// engine/src/simulation/src/movement_solver.cc -- move.x is world +X and move.y
// is world +Z). The script only decides *where the input points*; how fast the
// body yaws to face it stays with the kernel (max_yaw_degrees_per_second).
//
// Grammar (steps separated by ';', ',' or a newline):
//
//   +X | -X | +Z | -Z       set the heading absolutely
//   forward <n>[m]          hold the heading until the subject has travelled
//                           n metres in the XZ plane
//   turn <n>[deg]           rotate the heading n degrees (+X toward +Z is
//                           positive), applied instantly to the input
//   wait <n>[s]             zero move input for n seconds
//
// English keywords have Chinese aliases: 前進/前进 (forward), 旋轉/旋转/轉/转
// (turn), 等待/停 (wait). Whitespace between keyword and number is optional, so
// both "forward 10m" and "前進10m" parse.
//
// Examples:
//   "+X"                                 walk +X forever (the default)
//   "+X; forward 10m; turn 45; forward 5m"
//   "forward 10m; wait 2s; turn -90; forward 8m"
//
// A bare axis alias such as "+X" expands to that heading plus an unbounded
// forward step. Once every step is consumed the script reports finished() and
// returns a zero move input (the subject stands still) for the remaining
// samples.

#ifndef ENGINE_SRC_TESTS_CAPTURE_PATH_SCRIPT_H_
#define ENGINE_SRC_TESTS_CAPTURE_PATH_SCRIPT_H_

#include <cstddef>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace network_example::capture {

struct PathStep {
    enum class Kind {
        kSetHeading,
        kForward,
        kTurn,
        kWait,
    };

    Kind kind = Kind::kForward;
    float value = 0.0f;                  // metres, degrees or seconds.
    glm::vec2 heading{1.0f, 0.0f};       // kSetHeading only.
};

class PathScript {
  public:
    // `tick_rate_hz` converts wait durations into whole ticks.
    static bool parse(
        const std::string& text,
        float tick_rate_hz,
        PathScript* out_script,
        std::string* out_error);

    // Call once per tick with the subject's current world position. Returns the
    // move input for the next kernel update.
    glm::vec2 move_input(const glm::vec3& world_position);

    bool finished() const { return step_index_ >= steps_.size(); }
    const std::string& description() const { return description_; }
    const std::vector<PathStep>& steps() const { return steps_; }

  private:
    void enter_step(const glm::vec3& world_position);

    std::vector<PathStep> steps_;
    std::string description_;
    float tick_rate_hz_ = 30.0f;

    std::size_t step_index_ = 0;
    bool step_entered_ = false;
    glm::vec2 heading_{1.0f, 0.0f};
    glm::vec3 step_anchor_{0.0f, 0.0f, 0.0f};
    int wait_ticks_left_ = 0;
};

}  // namespace network_example::capture

#endif  // ENGINE_SRC_TESTS_CAPTURE_PATH_SCRIPT_H_
