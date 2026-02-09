#include "bridge/core/diagnostics.hpp"
#include <cstdio>
#include <cstdlib>
namespace bridge {
void Logger::write(Event event, std::uint64_t session, std::uint64_t value) {
    const char* name = "trace";
    switch (event) {
    case Event::session_started:
        name = "session_started";
        break;
    case Event::pairing_pending:
        name = "pairing_pending";
        break;
    case Event::paired:
        name = "paired";
        break;
    case Event::session_complete:
        name = "session_complete";
        break;
    case Event::session_failed:
        name = "session_failed";
        break;
    case Event::transfer_state:
        name = "transfer_state";
        break;
    case Event::durable_checkpoint:
        name = "durable_checkpoint";
        break;
    case Event::transfer_failed:
        name = "transfer_failed";
        break;
    case Event::trace:
        break;
    }
    output_ << "{\"event\":\"" << name << "\",\"session\":" << session << ",\"value\":" << value
            << "}\n";
    output_.flush();
}
TraceScope::~TraceScope() {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start_)
                             .count();
    logger_.write(Event::trace, session_, static_cast<std::uint64_t>(elapsed));
}
void check(bool condition, std::source_location location) {
    if (condition)
        return;
    std::fprintf(stderr, "{\"event\":\"invariant_failed\",\"line\":%u}\n", location.line());
    std::fflush(stderr);
    std::abort();
}
} // namespace bridge
