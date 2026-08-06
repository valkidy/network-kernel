#include "capture/path_script.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <limits>

namespace network_example::capture {
namespace {

constexpr float kPi = 3.14159265358979323846f;

std::string trim(const std::string& text) {
    const auto is_space = [](unsigned char value) {
        return std::isspace(value) != 0;
    };
    std::size_t begin = 0;
    while (begin < text.size() &&
           is_space(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           is_space(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string to_lower(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    return text;
}

// Recognizes "+X" / "x" / "-Z" style absolute headings.
bool parse_axis(const std::string& token, glm::vec2* out_heading) {
    std::string compact;
    for (const char value : token) {
        if (std::isspace(static_cast<unsigned char>(value)) == 0) {
            compact.push_back(value);
        }
    }
    compact = to_lower(compact);
    if (compact == "x" || compact == "+x") {
        *out_heading = glm::vec2(1.0f, 0.0f);
        return true;
    }
    if (compact == "-x") {
        *out_heading = glm::vec2(-1.0f, 0.0f);
        return true;
    }
    if (compact == "z" || compact == "+z") {
        *out_heading = glm::vec2(0.0f, 1.0f);
        return true;
    }
    if (compact == "-z") {
        *out_heading = glm::vec2(0.0f, -1.0f);
        return true;
    }
    return false;
}

bool starts_number(const std::string& text, std::size_t index) {
    const char value = text[index];
    if (std::isdigit(static_cast<unsigned char>(value)) != 0 || value == '.') {
        return true;
    }
    if ((value == '+' || value == '-') && index + 1 < text.size()) {
        const char next = text[index + 1];
        return std::isdigit(static_cast<unsigned char>(next)) != 0 ||
            next == '.';
    }
    return false;
}

bool keyword_matches(
    const std::string& keyword,
    std::initializer_list<const char*> aliases) {
    for (const char* alias : aliases) {
        if (keyword == alias) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> split_steps(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char value : text) {
        if (value == ';' || value == ',' || value == '\n') {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(value);
    }
    parts.push_back(current);
    return parts;
}

std::string describe(const std::vector<PathStep>& steps) {
    std::string text;
    for (const PathStep& step : steps) {
        if (!text.empty()) {
            text += "; ";
        }
        switch (step.kind) {
            case PathStep::Kind::kSetHeading:
                text += "heading(" + std::to_string(step.heading.x) + ", " +
                    std::to_string(step.heading.y) + ")";
                break;
            case PathStep::Kind::kForward:
                text += std::isfinite(step.value)
                    ? "forward " + std::to_string(step.value) + "m"
                    : std::string("forward (unbounded)");
                break;
            case PathStep::Kind::kTurn:
                text += "turn " + std::to_string(step.value) + "deg";
                break;
            case PathStep::Kind::kWait:
                text += "wait " + std::to_string(step.value) + "s";
                break;
        }
    }
    return text;
}

}  // namespace

bool PathScript::parse(
    const std::string& text,
    float tick_rate_hz,
    PathScript* out_script,
    std::string* out_error) {
    if (out_script == nullptr) {
        return false;
    }
    const auto fail = [out_error](std::string message) {
        if (out_error != nullptr) {
            *out_error = std::move(message);
        }
        return false;
    };
    if (!(tick_rate_hz > 0.0f)) {
        return fail("tick rate must be positive");
    }

    PathScript script;
    script.tick_rate_hz_ = tick_rate_hz;

    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        return fail("path is empty");
    }

    // A bare axis alias is the common case: walk that way for the whole run.
    glm::vec2 axis_heading{1.0f, 0.0f};
    if (parse_axis(trimmed, &axis_heading)) {
        script.steps_.push_back(PathStep{
            PathStep::Kind::kSetHeading, 0.0f, axis_heading});
        script.steps_.push_back(PathStep{
            PathStep::Kind::kForward,
            std::numeric_limits<float>::infinity(),
            glm::vec2(0.0f)});
        script.heading_ = axis_heading;
        script.description_ = trimmed;
        *out_script = std::move(script);
        return true;
    }

    for (const std::string& raw_step : split_steps(trimmed)) {
        const std::string step_text = trim(raw_step);
        if (step_text.empty()) {
            continue;
        }
        if (parse_axis(step_text, &axis_heading)) {
            script.steps_.push_back(PathStep{
                PathStep::Kind::kSetHeading, 0.0f, axis_heading});
            continue;
        }

        std::size_t number_begin = 0;
        while (number_begin < step_text.size() &&
               !starts_number(step_text, number_begin)) {
            ++number_begin;
        }
        if (number_begin >= step_text.size()) {
            return fail("path step '" + step_text + "' has no number");
        }
        const std::string keyword = to_lower(trim(step_text.substr(0, number_begin)));
        if (keyword.empty()) {
            return fail(
                "path step '" + step_text +
                "' has no keyword (expected forward/turn/wait)");
        }
        const std::string number_text = step_text.substr(number_begin);
        char* number_end = nullptr;
        const float value = std::strtof(number_text.c_str(), &number_end);
        if (number_end == number_text.c_str() || !std::isfinite(value)) {
            return fail("path step '" + step_text + "' has an invalid number");
        }

        PathStep step{};
        step.value = value;
        if (keyword_matches(keyword, {"forward", "fwd", "move", "前進", "前进"})) {
            if (value <= 0.0f) {
                return fail("forward distance must be positive: " + step_text);
            }
            step.kind = PathStep::Kind::kForward;
        } else if (keyword_matches(
                       keyword, {"turn", "rotate", "旋轉", "旋转", "轉", "转"})) {
            step.kind = PathStep::Kind::kTurn;
        } else if (keyword_matches(keyword, {"wait", "hold", "等待", "停"})) {
            if (value < 0.0f) {
                return fail("wait duration must not be negative: " + step_text);
            }
            step.kind = PathStep::Kind::kWait;
        } else {
            return fail("unknown path keyword '" + keyword + "' in '" + step_text + "'");
        }
        script.steps_.push_back(step);
    }

    if (script.steps_.empty()) {
        return fail("path has no steps");
    }
    script.description_ = describe(script.steps_);
    *out_script = std::move(script);
    return true;
}

void PathScript::enter_step(const glm::vec3& world_position) {
    const PathStep& step = steps_[step_index_];
    switch (step.kind) {
        case PathStep::Kind::kForward:
            step_anchor_ = world_position;
            break;
        case PathStep::Kind::kWait:
            wait_ticks_left_ = static_cast<int>(
                std::lround(static_cast<double>(step.value) * tick_rate_hz_));
            break;
        case PathStep::Kind::kSetHeading:
        case PathStep::Kind::kTurn:
            break;
    }
    step_entered_ = true;
}

glm::vec2 PathScript::move_input(const glm::vec3& world_position) {
    while (step_index_ < steps_.size()) {
        if (!step_entered_) {
            enter_step(world_position);
        }
        const PathStep& step = steps_[step_index_];
        const auto advance = [this]() {
            ++step_index_;
            step_entered_ = false;
        };
        switch (step.kind) {
            case PathStep::Kind::kSetHeading: {
                heading_ = step.heading;
                advance();
                break;
            }
            case PathStep::Kind::kTurn: {
                const float radians = step.value * kPi / 180.0f;
                const float cosine = std::cos(radians);
                const float sine = std::sin(radians);
                heading_ = glm::vec2(
                    heading_.x * cosine - heading_.y * sine,
                    heading_.x * sine + heading_.y * cosine);
                const float length = glm::length(heading_);
                if (length > 0.0f) {
                    heading_ /= length;
                }
                advance();
                break;
            }
            case PathStep::Kind::kWait: {
                if (wait_ticks_left_ <= 0) {
                    advance();
                    break;
                }
                --wait_ticks_left_;
                return glm::vec2(0.0f, 0.0f);
            }
            case PathStep::Kind::kForward: {
                const float dx = world_position.x - step_anchor_.x;
                const float dz = world_position.z - step_anchor_.z;
                const float travelled = std::sqrt(dx * dx + dz * dz);
                if (travelled >= step.value) {
                    advance();
                    break;
                }
                return heading_;
            }
        }
    }
    return glm::vec2(0.0f, 0.0f);
}

}  // namespace network_example::capture
