#pragma once
#include "bridge/core/diagnostics.hpp"
#include "bridge/core/session.hpp"
#include "bridge/net/transport.hpp"
#include <QObject>
namespace bridge::app {
class Session final : public QObject {
    Q_OBJECT
  public:
    Session(Role role, Logger& logger, QObject* parent = nullptr,
            SessionPurpose purpose = SessionPurpose::preview);
    ~Session() override { transport_.abort(); }
    Result<std::uint16_t> receive(const QHostAddress& address, std::uint16_t port);
    Result<void> connect_peer(const QHostAddress& address, std::uint16_t port);
    void confirm();
    void reject();
    void cancel();
    Result<void> send_application(Message type, std::span<const std::uint8_t> payload = {});
    void close_file();
    Result<void> expect_close();
    [[nodiscard]] bool can_confirm() const {
        return secure_ && state_.phase() == Phase::pairing && !confirmed_;
    }
    [[nodiscard]] bool paired() const { return state_.paired(); }
    [[nodiscard]] bool listening() const { return transport_.listening(); }
    [[nodiscard]] Phase phase() const { return state_.phase(); }
    // Test harness injects failures in the production transport, never through QML.
    net::Transport& diagnostic_transport() { return transport_; }
  signals:
    void pairing_pending(QString fingerprint);
    void changed();
    void ready();
    void application_frame(bridge::Frame frame);
    void finished(bool success, int error_code);

  private:
    void apply(Result<Actions> actions);
    void fail(Error error);
    SessionState state_;
    SessionPurpose purpose_;
    net::Transport transport_;
    Logger& logger_;
    std::uint64_t session_id_;
    bool secure_ = false;
    bool confirmed_ = false;
    bool terminal_ = false;
    bool ready_emitted_ = false;
    bool paired_logged_ = false;
};
} // namespace bridge::app
