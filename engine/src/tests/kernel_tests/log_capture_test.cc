// Kernel_PollLogMessages: the kernel's spdlog lines, handed to a host whose
// stdout nobody reads (the Unity Editor's does not reach Editor.log).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include "kernel/public/kernel_api.h"
#include "kernel/src/log_capture.h"

namespace {

void require_impl(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "log_capture_test:%d: requirement failed: %s\n", line, expression);
        std::exit(1);
    }
}

#define require(condition) require_impl((condition), #condition, __LINE__)

void keeps_the_newest_lines_and_numbers_them() {
    auto sink = std::make_shared<network_example::LogCaptureSink>(3u);
    spdlog::logger logger("capture", sink);
    logger.set_level(spdlog::level::trace);
    logger.info("line {}", 0);
    logger.info("line {}", 1);
    logger.warn("line {}", 2);
    logger.error("line {}", 3);
    logger.debug("line {}", 4);

    KernelLogMessage messages[8]{};
    require(sink->drain(messages, 2u) == 2u);
    // Two of five dropped, so the first kept line is sequence 2.
    require(messages[0].sequence == 2u);
    require(std::strcmp(messages[0].text, "line 2") == 0);
    require(messages[0].level == KernelLogLevel_Warn);
    require(messages[0].length == 6u);
    require(messages[0].truncated == 0u);
    require(messages[1].sequence == 3u);
    require(messages[1].level == KernelLogLevel_Error);

    require(sink->drain(messages, 8u) == 1u);
    require(std::strcmp(messages[0].text, "line 4") == 0);
    require(messages[0].level == KernelLogLevel_Debug);
    require(sink->drain(messages, 8u) == 0u);
    require(sink->drain(nullptr, 8u) == 0u);
}

void cuts_a_long_line_and_says_so() {
    auto sink = std::make_shared<network_example::LogCaptureSink>(4u);
    spdlog::logger logger("capture", sink);
    const std::string long_line(KERNEL_LOG_MESSAGE_TEXT_SIZE + 100u, 'x');
    logger.error("{}", long_line);

    KernelLogMessage message{};
    require(sink->drain(&message, 1u) == 1u);
    require(message.length == KERNEL_LOG_MESSAGE_TEXT_SIZE - 1u);
    require(message.truncated == 1u);
    require(message.text[KERNEL_LOG_MESSAGE_TEXT_SIZE - 1u] == '\0');
    require(message.text[KERNEL_LOG_MESSAGE_TEXT_SIZE - 2u] == 'x');
}

bool drained_line(const char* text, std::uint32_t level) {
    KernelLogMessage messages[16]{};
    bool found = false;
    std::uint32_t count = 0u;
    while ((count = Kernel_PollLogMessages(messages, 16u)) > 0u) {
        for (std::uint32_t index = 0u; index < count; ++index) {
            if (std::strcmp(messages[index].text, text) == 0 &&
                messages[index].level == level) {
                found = true;
            }
        }
    }
    return found;
}

void the_process_logger_is_captured_from_the_first_poll() {
    const std::size_t sinks_before = spdlog::default_logger()->sinks().size();
    spdlog::warn("before capture");
    require(Kernel_PollLogMessages(nullptr, 0u) == 0u);
    // Nothing from before the first poll.
    require(!drained_line("before capture", KernelLogLevel_Warn));

    spdlog::warn("captured {}", 7);
    require(drained_line("captured 7", KernelLogLevel_Warn));
    // Below the logger's level is not captured either.
    spdlog::debug("too quiet");
    require(!drained_line("too quiet", KernelLogLevel_Debug));
    // stdout still gets every line.
    require(spdlog::default_logger()->sinks().size() == sinks_before + 1u);

    // A second first-poll does not install a second sink.
    require(Kernel_PollLogMessages(nullptr, 0u) == 0u);
    require(spdlog::default_logger()->sinks().size() == sinks_before + 1u);
}

}  // namespace

int main() {
    keeps_the_newest_lines_and_numbers_them();
    cuts_a_long_line_and_says_so();
    the_process_logger_is_captured_from_the_first_poll();
    std::printf("log_capture_test passed\n");
    return 0;
}
