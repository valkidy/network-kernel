#ifndef KERNEL_SRC_LOG_CAPTURE_H_
#define KERNEL_SRC_LOG_CAPTURE_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <spdlog/sinks/base_sink.h>

#include "kernel/public/kernel_types.h"

namespace network_example {

// Keeps the newest `capacity` lines for a host to drain; the oldest are
// dropped once it is full. Behind Kernel_PollLogMessages.
class LogCaptureSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit LogCaptureSink(std::size_t capacity);

    // Moves up to max_messages lines, oldest first, into out_messages.
    std::uint32_t drain(KernelLogMessage* out_messages, std::uint32_t max_messages);

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override {}

private:
    struct Line {
        std::uint32_t level = 0;
        std::uint64_t sequence = 0;
        std::string text;
    };

    std::size_t capacity_;
    std::uint64_t next_sequence_ = 0;
    std::deque<Line> lines_;
};

// The sink behind the process's default logger, installed on the first call.
// Installing replaces the default logger with one that writes to the old
// sinks and to this one.
LogCaptureSink& process_log_capture();

}  // namespace network_example

#endif  // KERNEL_SRC_LOG_CAPTURE_H_
