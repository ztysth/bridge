#include "bridge/net/transport.hpp"
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QSslConfiguration>
#include <algorithm>
namespace bridge::net {
namespace {
constexpr qint64 buffer_limit = 16384;
Result<void> require_backend() {
    const auto backend = QStringLiteral("openssl");
    if ((QSslSocket::activeBackend() != backend && !QSslSocket::setActiveBackend(backend)) ||
        !QSslSocket::supportsSsl() || !QSslSocket::supportedProtocols().contains(QSsl::TlsV1_3))
        return std::unexpected(Error{ErrorCode::tls_failed});
    return {};
}
std::span<const std::uint8_t> bytes(const QByteArray& value) {
    return {reinterpret_cast<const std::uint8_t*>(value.constData()),
            static_cast<std::size_t>(value.size())};
}
} // namespace
bool is_local_address(const QHostAddress& address) {
    if (address.protocol() != QAbstractSocket::IPv4Protocol)
        return false;
    const auto value = address.toIPv4Address();
    return (value >> 24U) == 10 || (value >> 20U) == 0xac1 || (value >> 16U) == 0xc0a8 ||
           (value >> 16U) == 0xa9fe || (value >> 24U) == 127;
}
bool is_bind_address(const QHostAddress& address) {
    if (!is_local_address(address))
        return false;
    if (address.isLoopback())
        return true;
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp))
            continue;
        for (const auto& entry : interface.addressEntries())
            if (entry.ip() == address)
                return true;
    }
    return false;
}
void Listener::incomingConnection(qintptr descriptor) {
    close();
    accepted(descriptor);
}
Transport::Transport(QObject* parent) : QObject(parent) {
    listener_.setProxy(QNetworkProxy::NoProxy);
    listener_.setMaxPendingConnections(1);
    deadline_.setSingleShot(true);
    QObject::connect(&deadline_, &QTimer::timeout, this,
                     [this] { fail(Error{ErrorCode::timeout}); });
    listener_.accepted = [this](qintptr descriptor) { accept(descriptor); };
}
Result<void> Transport::prepare() {
    if (socket_ || stopped_)
        return std::unexpected(Error{ErrorCode::invalid_state});
    auto backend = require_backend();
    if (!backend)
        return backend;
    auto identity = security::make_identity();
    if (!identity)
        return std::unexpected(identity.error());
    identity_ = std::move(*identity);
    socket_ = std::make_unique<QSslSocket>();
    socket_->setProxy(QNetworkProxy::NoProxy);
    socket_->setReadBufferSize(buffer_limit);
    auto config = socket_->sslConfiguration();
    config.setProtocol(QSsl::TlsV1_3);
    config.setPeerVerifyMode(QSslSocket::VerifyPeer);
    config.setPeerVerifyDepth(1);
    config.setLocalCertificate(identity_->certificate);
    config.setPrivateKey(identity_->key);
    config.setSslOption(QSsl::SslOptionDisableSessionTickets, true);
    config.setSslOption(QSsl::SslOptionDisableSessionSharing, true);
    config.setSslOption(QSsl::SslOptionDisableSessionPersistence, true);
    socket_->setSslConfiguration(config);
    socket_->setPeerVerifyName(QStringLiteral("bridge.local"));
    QObject::connect(socket_.get(), &QSslSocket::sslErrors, this,
                     [this](const QList<QSslError>& errors) {
                         const auto peer = socket_->peerCertificate();
                         if (!security::validate_peer(peer) || errors.empty() ||
                             !std::ranges::all_of(errors, [&peer](const QSslError& error) {
                                 return error.error() == QSslError::SelfSignedCertificate &&
                                        error.certificate() == peer;
                             })) {
                             fail(Error{ErrorCode::tls_failed});
                             return;
                         }
                         socket_->ignoreSslErrors(
                             errors); // Explicit pairing replaces this one PKI-chain exception.
                     });
    QObject::connect(socket_.get(), &QSslSocket::encrypted, this, [this] {
        if (stopped_)
            return;
        const auto peer = socket_->peerCertificate();
        if (socket_->sessionProtocol() != QSsl::TlsV1_3) {
            fail(Error{ErrorCode::tls_failed});
            return;
        }
        auto fingerprint = security::pairing_fingerprint(server_ ? peer : identity_->certificate,
                                                         server_ ? identity_->certificate : peer);
        if (!fingerprint) {
            fail(fingerprint.error());
            return;
        }
        set_deadline(120000);
        emit secured(*fingerprint);
    });
    QObject::connect(socket_.get(), &QSslSocket::readyRead, this, [this] { read_frames(); });
    QObject::connect(socket_.get(), &QSslSocket::disconnected, this, [this] {
        if (stopped_)
            return;
        deadline_.stop();
        stopped_ = true;
        if (!incoming_.isEmpty())
            emit failed(Error{ErrorCode::malformed_frame});
        else
            emit disconnected();
    });
    QObject::connect(socket_.get(), &QSslSocket::errorOccurred, this,
                     [this](QAbstractSocket::SocketError error) {
                         // RemoteHostClosedError is followed by disconnected; app decides whether
                         // it is expected.
                         if (error != QAbstractSocket::RemoteHostClosedError)
                             fail(Error{error == QAbstractSocket::SslHandshakeFailedError
                                            ? ErrorCode::tls_failed
                                            : ErrorCode::connect_failed});
                     });
    return {};
}
Result<std::uint16_t> Transport::receive(const QHostAddress& address, std::uint16_t port) {
    if (socket_ || stopped_ || listener_.isListening())
        return std::unexpected(Error{ErrorCode::invalid_state});
    if (!is_bind_address(address))
        return std::unexpected(Error{ErrorCode::invalid_address});
    auto backend = require_backend();
    if (!backend)
        return std::unexpected(backend.error());
    server_ = true;
    if (!listener_.listen(address, port))
        return std::unexpected(Error{ErrorCode::listen_failed});
    set_deadline(120000);
    return listener_.serverPort();
}
void Transport::accept(qintptr descriptor) {
    auto prepared = prepare();
    if (!prepared) {
        QTcpSocket rejected; // Immediately owns and closes the accepted descriptor.
        if (!rejected.setSocketDescriptor(descriptor)) {
            fail(Error{ErrorCode::connect_failed});
            return;
        }
        fail(prepared.error());
        return;
    }
    if (!socket_->setSocketDescriptor(descriptor)) {
        fail(Error{ErrorCode::connect_failed});
        return;
    }
    if (!is_local_address(socket_->peerAddress())) {
        fail(Error{ErrorCode::invalid_address});
        return;
    }
    set_deadline(10000);
    socket_->startServerEncryption();
}
Result<void> Transport::connect_peer(const QHostAddress& address, std::uint16_t port) {
    if (!is_local_address(address) || port == 0)
        return std::unexpected(Error{ErrorCode::invalid_address});
    auto prepared = prepare();
    if (!prepared)
        return prepared;
    set_deadline(10000);
    socket_->connectToHostEncrypted(address.toString(), port, QStringLiteral("bridge.local"));
    return {};
}
void Transport::set_deadline(int milliseconds) { deadline_.start(milliseconds); }
Result<void> Transport::send_bytes_for_test(std::span<const std::uint8_t> data) {
    if (stopped_ || !encrypted())
        return std::unexpected(Error{ErrorCode::invalid_state});
    const qint64 limit =
        file_frames_ ? 2 * (max_file_payload + header_size) + buffer_limit : buffer_limit;
    if (data.size() > static_cast<std::size_t>(limit) ||
        socket_->bytesToWrite() > limit - static_cast<qint64>(data.size()) ||
        socket_->encryptedBytesToWrite() > limit - static_cast<qint64>(data.size()))
        return std::unexpected(Error{ErrorCode::queue_full});
    if (socket_->write(reinterpret_cast<const char*>(data.data()),
                       static_cast<qint64>(data.size())) != static_cast<qint64>(data.size()))
        return std::unexpected(Error{ErrorCode::disconnected});
    return {};
}
Result<void> Transport::send(Message type, std::span<const std::uint8_t> payload) {
    if (is_file_message(type) && !file_frames_)
        return std::unexpected(Error{ErrorCode::invalid_state});
    auto encoded = encode_frame(type, payload);
    if (!encoded)
        return std::unexpected(encoded.error());
    return send_bytes_for_test(*encoded);
}
void Transport::read_frames() {
    if (stopped_ || !encrypted())
        return;
    // One exact frame at a time, validating the header before asking for body bytes.
    unsigned processed = 0;
    while (socket_->bytesAvailable() > 0 && !stopped_) {
        const auto needed = expected_size_ - static_cast<std::size_t>(incoming_.size());
        incoming_.append(socket_->read(static_cast<qint64>(needed)));
        if (static_cast<std::size_t>(incoming_.size()) != expected_size_)
            return;
        if (expected_size_ == header_size) {
            auto header = decode_header(bytes(incoming_));
            if (!header) {
                fail(header.error());
                return;
            }
            if (is_file_message(header->type) && !file_frames_) {
                fail(Error{ErrorCode::invalid_state});
                return;
            }
            expected_size_ = header_size + header->length;
            if (header->length != 0)
                continue;
        }
        auto frame = decode_frame(bytes(incoming_));
        if (!frame) {
            fail(frame.error());
            return;
        }
        incoming_.clear();
        expected_size_ = header_size;
        emit frame_received(std::move(*frame));
        if (++processed == 32 && !stopped_) {
            QTimer::singleShot(0, this, [this] { read_frames(); });
            return;
        }
    }
}
void Transport::enable_file_frames() {
    file_frames_ = true;
    socket_->setReadBufferSize(2 * (max_file_payload + header_size) + buffer_limit);
}
void Transport::shutdown() {
    if (socket_ && !stopped_)
        socket_->disconnectFromHost();
}
void Transport::abort() {
    stopped_ = true;
    deadline_.stop();
    listener_.close();
    if (socket_)
        socket_->abort();
}
void Transport::fail(Error error) {
    if (stopped_)
        return;
    abort();
    emit failed(error);
}
} // namespace bridge::net
