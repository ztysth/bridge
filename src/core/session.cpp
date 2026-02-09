#include "bridge/core/session.hpp"
#include <algorithm>
namespace bridge {
Actions SessionState::advance_pairing() {
    if (!paired())
        return {};
    if (role_ == Role::receiver) {
        phase_ = Phase::hello;
        return {};
    }
    phase_ = Phase::hello_ack;
    return Actions{{Message::hello}, 1, false};
}
Result<Actions> SessionState::confirm() {
    if (phase_ != Phase::pairing || local_confirmed_)
        return std::unexpected(Error{ErrorCode::invalid_state});
    local_confirmed_ = true;
    auto next = advance_pairing();
    Actions result{{Message::confirm}, 1, false};
    if (next.count != 0)
        result.messages[result.count++] = next.messages[0];
    return result;
}
Result<Actions> SessionState::receive(const Frame& frame) {
    const auto valid = validate_payload(frame.type, frame.payload);
    if (!valid)
        return std::unexpected(valid.error());
    if (phase_ == Phase::complete || phase_ == Phase::failed || phase_ == Phase::closing)
        return std::unexpected(Error{ErrorCode::invalid_state});
    if (frame.type == Message::cancel)
        return std::unexpected(Error{ErrorCode::cancelled});
    if (frame.type == Message::error)
        return std::unexpected(Error{ErrorCode::peer_error});
    if (phase_ == Phase::pairing && frame.type == Message::confirm && !remote_confirmed_) {
        remote_confirmed_ = true;
        return advance_pairing();
    }
    if (!paired())
        return std::unexpected(Error{ErrorCode::invalid_state});
    switch (phase_) {
    case Phase::hello:
        if (frame.type == Message::hello && frame.payload.size() == 4) {
            const auto minimum = read_u16(std::span<const std::uint8_t>(frame.payload).first<2>());
            const auto maximum =
                read_u16(std::span<const std::uint8_t>(frame.payload).subspan<2, 2>());
            if (purpose_ == SessionPurpose::file && (minimum != 2 || maximum != 2))
                return std::unexpected(Error{ErrorCode::unsupported_version});
            if (minimum > (purpose_ == SessionPurpose::file ? 2 : 0) ||
                maximum < (purpose_ == SessionPurpose::file ? 2 : 0) || minimum > maximum)
                return std::unexpected(Error{ErrorCode::unsupported_version});
            phase_ = purpose_ == SessionPurpose::file ? Phase::ready : Phase::ping;
            return Actions{{Message::hello_ack}, 1, false};
        }
        break;
    case Phase::hello_ack:
        if (frame.type == Message::hello_ack && frame.payload.size() == 2) {
            if (read_u16(std::span<const std::uint8_t>(frame.payload).first<2>()) !=
                (purpose_ == SessionPurpose::file ? 2 : 0))
                return std::unexpected(Error{ErrorCode::unsupported_version});
            if (purpose_ == SessionPurpose::file) {
                phase_ = Phase::ready;
                return Actions{};
            }
            phase_ = Phase::pong;
            return Actions{{Message::ping}, 1, false};
        }
        break;
    case Phase::ping:
    case Phase::pong:
        if (frame.type == (phase_ == Phase::ping ? Message::ping : Message::pong) &&
            std::ranges::equal(frame.payload, ping_payload)) {
            const bool receiver = phase_ == Phase::ping;
            phase_ = receiver ? Phase::close : Phase::close_ack;
            return Actions{{receiver ? Message::pong : Message::close}, 1, false};
        }
        break;
    case Phase::close:
        if (frame.type == Message::close && frame.payload.empty()) {
            phase_ = Phase::closing;
            return Actions{{Message::close_ack}, 1, true};
        }
        break;
    case Phase::close_ack:
        if (frame.type == Message::close_ack && frame.payload.empty()) {
            phase_ = Phase::closing;
            return Actions{{}, 0, true};
        }
        break;
    default:
        break;
    }
    return std::unexpected(Error{ErrorCode::invalid_state});
}
Result<Actions> SessionState::close_file() {
    if (purpose_ != SessionPurpose::file || role_ != Role::initiator || phase_ != Phase::ready)
        return std::unexpected(Error{ErrorCode::invalid_state});
    phase_ = Phase::close_ack;
    return Actions{{Message::close}, 1, false};
}
Result<void> SessionState::expect_close() {
    if (purpose_ != SessionPurpose::file || role_ != Role::receiver || phase_ != Phase::ready)
        return std::unexpected(Error{ErrorCode::invalid_state});
    phase_ = Phase::close;
    return {};
}
Result<void> SessionState::disconnected() {
    if (phase_ != Phase::closing) {
        phase_ = Phase::failed;
        return std::unexpected(Error{ErrorCode::disconnected});
    }
    phase_ = Phase::complete;
    return {};
}
} // namespace bridge
