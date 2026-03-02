#include "model.hpp"
#include <QFileInfo>
#include <QNetworkInterface>
#include <iostream>
SessionModel::SessionModel(QObject* parent) : QObject(parent), logger_(std::cerr) {
    if (!transfer_supported())
        status_ = QStringLiteral("File transfer is not available on Windows yet.");
}
QStringList SessionModel::local_addresses() const {
    QStringList result{QStringLiteral("127.0.0.1")};
    for (const auto& address : QNetworkInterface::allAddresses()) {
        if (bridge::net::is_bind_address(address) && !result.contains(address.toString()))
            result.append(address.toString());
    }
    return result;
}
bool SessionModel::create(bridge::Role role, const QString& address, const QString& port,
                          std::uint16_t& parsed_port) {
    if (busy())
        return false;
    bool ok = false;
    const auto number = port.toUInt(&ok);
    if (!ok || number > 65535 || !bridge::net::is_local_address(QHostAddress(address))) {
        show_error(bridge::Error{bridge::ErrorCode::invalid_address});
        return false;
    }
    parsed_port = static_cast<std::uint16_t>(number);
    fingerprint_.clear();
    endpoint_ = address;

    role_sender_ = role == bridge::Role::initiator;
    transfer_ = std::make_unique<bridge::app::Transfer>(role, logger_);
    QObject::connect(
        transfer_.get(), &bridge::app::Transfer::pairing_pending, this,
        [this](const QString& fingerprint) {
            fingerprint_ = fingerprint;
            status_ = QStringLiteral(
                "Authentication pending. Compare every fingerprint digit with the other screen.");
            emit changed();
        });
    QObject::connect(transfer_.get(), &bridge::app::Transfer::changed, this, [this] {
        if (transfer_->manifest() && role_sender_) {
            saved_manifest_ = transfer_->manifest();
        }
        status_ = transfer_->status();
        emit changed();
    });
    QObject::connect(transfer_.get(), &bridge::app::Transfer::finished, this,
                     [this](bool success, int code) {
                         Q_UNUSED(success)
                         Q_UNUSED(code)
                         status_ = transfer_->status();
                         emit changed();
                     });
    return true;
}
void SessionModel::show_error(bridge::Error error) {
    status_ = QString::fromUtf8(bridge::error_message(error.code).data());
    emit changed();
}
void SessionModel::receive(const QString& address, const QString& port, const QString& destination,
                           bool resume) {
    destination_ = destination;
    std::uint16_t number = 0;
    if (!create(bridge::Role::receiver, address, port, number))
        return;
    auto result = transfer_->receive(QHostAddress(address), number, destination.toStdString(),
                                     resume ? bridge::app::ReceiveMode::resume
                                            : bridge::app::ReceiveMode::create);
    if (!result) {
        show_error(result.error());
        return;
    }
    endpoint_ = address + ":" + QString::number(*result);
    status_ = QStringLiteral(
        "Receive mode enabled for one session. Share the endpoint with the intended peer.");
    emit changed();
}
void SessionModel::connectPeer(const QString& address, const QString& port, const QString& source,
                               bool resume) {
    if (resume && (!saved_manifest_ || source != saved_source_)) {
        show_error({bridge::ErrorCode::invalid_checkpoint});
        return;
    }
    std::uint16_t number = 0;
    if (!create(bridge::Role::initiator, address, port, number))
        return;
    auto result = transfer_->send_file(QHostAddress(address), number, source.toStdString(),
                                       resume ? saved_manifest_ : std::nullopt);
    if (!result) {
        show_error(result.error());
        return;
    }
    if (!resume)
        saved_manifest_.reset();
    saved_source_ = source;
    endpoint_ = address + ":" + QString::number(number);
    status_ = QStringLiteral("Connecting securely. The peer is not authenticated yet.");
    emit changed();
}
void SessionModel::confirm() {
    if (transfer_)
        transfer_->confirm();
}
void SessionModel::reject() {
    if (transfer_)
        transfer_->reject();
}
void SessionModel::cancel() {
    if (transfer_)
        transfer_->cancel();
}

QString SessionModel::filename() const {
    return transfer_ && transfer_->manifest() ? QString::fromStdString(transfer_->manifest()->name)
                                              : QString{};
}
QString SessionModel::checkpoint() const {
    if (!transfer_ || !transfer_->manifest())
        return {};
    return QStringLiteral("%1 / %2 bytes durably saved")
        .arg(transfer_->durable_bytes())
        .arg(transfer_->manifest()->size);
}
double SessionModel::progress() const {
    if (!transfer_ || !transfer_->manifest())
        return 0;
    if (transfer_->manifest()->size == 0)
        return transfer_->phase() == bridge::app::TransferPhase::complete ? 1 : 0;
    return static_cast<double>(transfer_->durable_bytes()) /
           static_cast<double>(transfer_->manifest()->size);
}
void SessionModel::acceptFile() {
    if (transfer_) {
        auto r = transfer_->accept();
        if (!r)
            show_error(r.error());
    }
}
void SessionModel::pause() {
    if (transfer_) {
        auto r = transfer_->pause();
        if (!r)
            show_error(r.error());
    }
}
void SessionModel::continueTransfer() {
    if (transfer_) {
        auto r = transfer_->continue_transfer();
        if (!r)
            show_error(r.error());
    }
}

namespace {
QString local_selection(const QUrl& url) {
    if (!url.isLocalFile() || !url.host().isEmpty() || url.hasQuery() || url.hasFragment())
        return {};
    const auto path = url.toLocalFile();
    if (path.isEmpty() || path.size() > 4096 || path.contains(QChar(0)))
        return {};
    return path;
}
} // namespace
bool SessionModel::dropUrls(const QList<QUrl>& urls) {
    if (busy())
        return false;
    if (urls.size() != 1) {
        status_ =
            QStringLiteral("Drop one file or folder at a time. Put multiple items into a folder.");
        emit changed();
        return false;
    }
    return selectSource(urls.front());
}
bool SessionModel::selectSource(const QUrl& url) {
    if (busy())
        return false;
    auto path = local_selection(url);
    if (path.isEmpty()) {
        status_ = QStringLiteral("Choose a local file or folder. Remote URLs cannot be sent.");
        emit changed();
        return false;
    }
    selected_source_ = path;
    status_ =
        QStringLiteral("Selection ready. Enter the receiver’s local address and port to send.");
    emit changed();
    return true;
}
bool SessionModel::selectDestination(const QUrl& url) {
    if (busy())
        return false;
    auto path = local_selection(url);
    if (path.isEmpty()) {
        status_ = QStringLiteral("Choose a local destination folder.");
        emit changed();
        return false;
    }
    destination_ = path;
    emit changed();
    return true;
}
void SessionModel::sendSelected(const QString& address, const QString& port, bool resume) {
    if (selected_source_.isEmpty())
        return;
    connectPeer(address, port, selected_source_, resume);
}
QString SessionModel::selected_name() const { return QFileInfo(selected_source_).fileName(); }
QString SessionModel::phase_title() const {
    if (!transfer_)
        return QStringLiteral("Ready");
    using bridge::app::TransferPhase;
    switch (transfer_->phase()) {
    case TransferPhase::idle:
        return QStringLiteral("Ready");
    case TransferPhase::preparing:
        return QStringLiteral("Preparing");
    case TransferPhase::pairing:
        return QStringLiteral("Confirm peer");
    case TransferPhase::awaiting_offer:
        return QStringLiteral("Ready to receive");
    case TransferPhase::awaiting_accept:
        return QStringLiteral("Waiting for approval");
    case TransferPhase::transferring:
        return QStringLiteral("Transferring");
    case TransferPhase::pausing:
        return QStringLiteral("Pausing");
    case TransferPhase::paused:
        return QStringLiteral("Paused");
    case TransferPhase::verifying:
        return QStringLiteral("Verifying");
    case TransferPhase::closing:
        return QStringLiteral("Closing");
    case TransferPhase::complete:
        return QStringLiteral("Complete");
    case TransferPhase::failed:
        return QStringLiteral("Transfer stopped");
    }
    return {};
}
