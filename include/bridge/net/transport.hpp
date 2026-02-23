#pragma once
#include "bridge/core/frame.hpp"
#include "bridge/security/identity.hpp"
#include <QObject>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
namespace bridge::net {
bool is_local_address(const QHostAddress& address);
bool is_bind_address(const QHostAddress& address);
class Listener final : public QTcpServer {
  public:
    std::function<void(qintptr)> accepted;

  private:
    void incomingConnection(qintptr descriptor) override;
};
class Transport final : public QObject {
    Q_OBJECT
  public:
    explicit Transport(QObject* parent = nullptr);
    ~Transport() override { abort(); }
    Result<std::uint16_t> receive(const QHostAddress& address, std::uint16_t port);
    Result<void> connect_peer(const QHostAddress& address, std::uint16_t port);
    Result<void> send(Message type, std::span<const std::uint8_t> payload = {});
    void set_deadline(int milliseconds);
    void enable_file_frames();
    void shutdown();
    void abort();
    [[nodiscard]] bool listening() const { return listener_.isListening(); }
    [[nodiscard]] bool encrypted() const { return socket_ && socket_->isEncrypted(); }
    // Diagnostic seam: caller must own the event loop and choose a finite duration.
    void expire_for_test() { fail(Error{ErrorCode::timeout}); }
    Result<void> send_bytes_for_test(std::span<const std::uint8_t> bytes);
  signals:
    void secured(QString fingerprint);
    void frame_received(bridge::Frame frame);
    void disconnected();
    void failed(bridge::Error error);

  private:
    Result<void> prepare();
    void accept(qintptr descriptor);
    void read_frames();
    void fail(Error error);
    Listener listener_;
    QTimer deadline_;
    std::unique_ptr<QSslSocket> socket_;
    std::optional<security::Identity> identity_;
    QByteArray incoming_;
    std::size_t expected_size_ = header_size;
    bool file_frames_ = false;
    bool server_ = false;
    bool stopped_ = false;
};
} // namespace bridge::net
