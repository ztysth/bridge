#include "bridge/app/session.hpp"
#include <QRandomGenerator>
namespace bridge::app {
Session::Session(Role role, Logger& logger, QObject* parent, SessionPurpose purpose)
    : QObject(parent), state_(role, purpose), purpose_(purpose), logger_(logger),
      session_id_(QRandomGenerator::system()->generate64()) {
    QObject::connect(&transport_, &net::Transport::secured, this,
                     [this](const QString& fingerprint) {
                         secure_ = true;
                         logger_.write(Event::pairing_pending, session_id_);
                         emit pairing_pending(fingerprint);
                         emit changed();
                     });
    QObject::connect(&transport_, &net::Transport::frame_received, this,
                     [this](const Frame& frame) {
                         if (terminal_)
                             return;
                         if (is_file_message(frame.type) && ready_emitted_ &&
                             (state_.phase() == Phase::ready || frame.type == Message::heartbeat)) {
                             transport_.set_deadline(15000);
                             emit application_frame(frame);
                         } else
                             apply(state_.receive(frame));
                     });
    QObject::connect(&transport_, &net::Transport::failed, this,
                     [this](Error error) { fail(error); });
    QObject::connect(&transport_, &net::Transport::disconnected, this, [this] {
        auto result = state_.disconnected();
        if (!result) {
            fail(result.error());
            return;
        }
        terminal_ = true;
        logger_.write(Event::session_complete, session_id_);
        emit changed();
        emit finished(true, 0);
    });
}
Result<std::uint16_t> Session::receive(const QHostAddress& address, std::uint16_t port) {
    logger_.write(Event::session_started, session_id_);
    return transport_.receive(address, port);
}
Result<void> Session::connect_peer(const QHostAddress& address, std::uint16_t port) {
    logger_.write(Event::session_started, session_id_);
    return transport_.connect_peer(address, port);
}
void Session::confirm() {
    if (!can_confirm()) {
        fail(Error{ErrorCode::invalid_state});
        return;
    }
    confirmed_ = true;
    apply(state_.confirm());
}
void Session::reject() { fail(Error{ErrorCode::rejected}); }
void Session::cancel() { fail(Error{ErrorCode::cancelled}); }
void Session::apply(Result<Actions> actions) {
    if (terminal_)
        return;
    if (!actions) {
        fail(actions.error());
        return;
    }
    for (std::size_t i = 0; i < actions->count; ++i) {
        const auto message = actions->messages[i];
        const std::uint8_t minor = purpose_ == SessionPurpose::file ? 2 : 0;
        std::array<std::uint8_t, 4> hello{0, minor, 0, minor};
        std::span<const std::uint8_t> payload;
        if (message == Message::hello)
            payload = hello;
        else if (message == Message::hello_ack)
            payload = std::span<const std::uint8_t>(hello).first<2>();
        else if (message == Message::ping || message == Message::pong)
            payload = ping_payload;
        auto result = transport_.send(message, payload);
        if (!result) {
            fail(result.error());
            return;
        }
    }
    if (state_.paired()) {
        transport_.set_deadline(10000);
        if (!paired_logged_) {
            paired_logged_ = true;
            logger_.write(Event::paired, session_id_);
        }
    }
    if (state_.phase() == Phase::ready && !ready_emitted_) {
        ready_emitted_ = true;
        transport_.enable_file_frames();
        transport_.set_deadline(15000);
        emit ready();
    }
    if (actions->shutdown)
        transport_.shutdown();
    emit changed();
}
Result<void> Session::send_application(Message type, std::span<const std::uint8_t> payload) {
    if (terminal_ || !ready_emitted_ || !is_file_message(type) ||
        (state_.phase() != Phase::ready && type != Message::heartbeat))
        return std::unexpected(Error{ErrorCode::invalid_state});
    return transport_.send(type, payload);
}
void Session::close_file() { apply(state_.close_file()); }
Result<void> Session::expect_close() { return state_.expect_close(); }
void Session::fail(Error error) {
    if (terminal_)
        return;
    terminal_ = true;
    state_.fail();
    transport_.abort();
    logger_.write(Event::session_failed, session_id_, static_cast<std::uint16_t>(error.code));
    emit changed();
    emit finished(false, static_cast<int>(error.code));
}
} // namespace bridge::app
