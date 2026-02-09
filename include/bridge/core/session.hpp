#pragma once
#include "bridge/core/frame.hpp"
#include <array>
namespace bridge {
enum class Role { initiator, receiver };
enum class SessionPurpose { preview, file };
enum class Phase {
    pairing,
    ready,
    hello,
    hello_ack,
    ping,
    pong,
    close,
    close_ack,
    closing,
    complete,
    failed
};
struct Actions {
    std::array<Message, 2> messages{};
    std::size_t count = 0;
    bool shutdown = false;
};
class SessionState {
  public:
    explicit SessionState(Role role, SessionPurpose purpose = SessionPurpose::preview)
        : role_(role), purpose_(purpose) {}
    Result<Actions> confirm();
    Result<Actions> receive(const Frame& frame);
    Result<void> disconnected();
    Result<Actions> close_file();
    Result<void> expect_close();
    void fail() { phase_ = Phase::failed; }
    [[nodiscard]] Phase phase() const { return phase_; }
    [[nodiscard]] bool paired() const { return local_confirmed_ && remote_confirmed_; }

  private:
    Actions advance_pairing();
    Role role_;
    SessionPurpose purpose_;
    Phase phase_ = Phase::pairing;
    bool local_confirmed_ = false;
    bool remote_confirmed_ = false;
};
} // namespace bridge
