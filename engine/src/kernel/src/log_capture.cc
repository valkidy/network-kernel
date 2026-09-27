#include "kernel/src/log_capture.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>

namespace network_example {
namespace {

// About a minute of a busy server's lines at the rate a play log shows; a
// host draining every frame never comes near it.
constexpr std::size_t kProcessLogCaptureCapacity = 1024;

}  // namespace

LogCaptureSink::LogCaptureSink(std::size_t capacity)
    : capacity_(std::max<std::size_t>(capacity, 1)) {}

void LogCaptureSink::sink_it_(const spdlog::details::log_msg& msg) {
    if (lines_.size() == capacity_) {
        lines_.pop_front();
    }
    Line line;
    line.level = static_cast<std::uint32_t>(msg.level);
    line.sequence = next_sequence_++;
    line.text.assign(msg.payload.data(), msg.payload.size());
    lines_.push_back(std::move(line));
}

std::uint32_t LogCaptureSink::drain(
    KernelLogMessage* out_messages,
    std::uint32_t max_messages) {
    if (out_messages == nullptr || max_messages == 0u) {
        return 0u;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint32_t count = 0u;
    while (count < max_messages && !lines_.empty()) {
        const Line& line = lines_.front();
        KernelLogMessage& out = out_messages[count];
        out = KernelLogMessage{};
        out.level = line.level;
        out.sequence = line.sequence;
        const std::size_t length =
            std::min<std::size_t>(line.text.size(), KERNEL_LOG_MESSAGE_TEXT_SIZE - 1u);
        std::memcpy(out.text, line.text.data(), length);
        out.text[length] = '\0';
        out.length = static_cast<std::uint32_t>(length);
        out.truncated = length < line.text.size() ? 1u : 0u;
        lines_.pop_front();
        ++count;
    }
    return count;
}

LogCaptureSink& process_log_capture() {
    // Never destroyed: the logger it is installed in outlives any caller.
    static LogCaptureSink* const sink = [] {
        auto capture = std::make_shared<LogCaptureSink>(kProcessLogCaptureCapacity);
        std::shared_ptr<spdlog::logger> previous = spdlog::default_logger();
        std::vector<spdlog::sink_ptr> sinks = previous->sinks();
        sinks.push_back(capture);
        auto logger = std::make_shared<spdlog::logger>(previous->name(), sinks.begin(), sinks.end());
        logger->set_level(previous->level());
        logger->flush_on(previous->flush_level());
        spdlog::set_default_logger(logger);
        // spdlog's free functions call through a raw pointer to the default
        // logger. A thread already inside one keeps using the old logger, so
        // it must not be freed under it.
        static std::shared_ptr<spdlog::logger> keep_previous_alive = std::move(previous);
        static std::shared_ptr<LogCaptureSink> keep_capture_alive = capture;
        return capture.get();
    }();
    return *sink;
}

}  // namespace network_example
