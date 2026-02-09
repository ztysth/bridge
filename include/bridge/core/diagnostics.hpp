#pragma once
#include "bridge/core/error.hpp"
#include <chrono>
#include <cstdint>
#include <ostream>
#include <source_location>
namespace bridge {
enum class Event {
    session_started,
    pairing_pending,
    paired,
    session_complete,
    session_failed,
    trace,
    transfer_state,
    durable_checkpoint,
    transfer_failed
};
class Logger {
  public:
    // The output stream must outlive the logger and its traces.
    explicit Logger(std::ostream& output) : output_(output) {}
    void write(Event event, std::uint64_t session, std::uint64_t value = 0);

  private:
    std::ostream& output_;
};
class TraceScope {
  public:
    TraceScope(Logger& logger, std::uint64_t session) : logger_(logger), session_(session) {}
    ~TraceScope();
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;

  private:
    Logger& logger_;
    std::uint64_t session_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};
class Failpoint {
  public:
    explicit Failpoint(std::uint32_t fail_on_hit = 0) : remaining_(fail_on_hit) {}
    bool hit() {
        if (remaining_ == 0)
            return false;
        return --remaining_ == 0;
    }

  private:
    std::uint32_t remaining_;
};
void check(bool condition, std::source_location location = std::source_location::current());
} // namespace bridge
#define BRIDGE_CHECK(condition) ::bridge::check(static_cast<bool>(condition))
#ifndef NDEBUG
#define BRIDGE_DCHECK(condition) BRIDGE_CHECK(condition)
#else
#define BRIDGE_DCHECK(condition) static_cast<void>(0)
#endif
#define BRIDGE_ENSURE(condition, code)                                                             \
    do {                                                                                           \
        if (!(condition))                                                                          \
            return std::unexpected(::bridge::Error{code});                                         \
    } while (false)
#define BRIDGE_TRACE_SCOPE(logger, session)                                                        \
    ::bridge::TraceScope bridge_trace_scope { logger, session }
