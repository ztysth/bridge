#pragma once
#include "bridge/app/transfer.hpp"
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <memory>
class SessionModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString selectedSource READ selected_source NOTIFY changed)
    Q_PROPERTY(QString selectedName READ selected_name NOTIFY changed)
    Q_PROPERTY(QString destination READ destination NOTIFY changed)
    Q_PROPERTY(QString phaseTitle READ phase_title NOTIFY changed)
    Q_PROPERTY(bool complete READ complete NOTIFY changed)
    Q_PROPERTY(bool offeredFolder READ offered_folder NOTIFY changed)
    Q_PROPERTY(bool transferSupported READ transfer_supported CONSTANT)
    Q_PROPERTY(QString filename READ filename NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString checkpoint READ checkpoint NOTIFY changed)
    Q_PROPERTY(bool canAccept READ can_accept NOTIFY changed)
    Q_PROPERTY(bool canPause READ can_pause NOTIFY changed)
    Q_PROPERTY(bool canContinue READ can_continue NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString fingerprint READ fingerprint NOTIFY changed)
    Q_PROPERTY(QString endpoint READ endpoint NOTIFY changed)
    Q_PROPERTY(QStringList localAddresses READ local_addresses CONSTANT)
    Q_PROPERTY(bool canConfirm READ can_confirm NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool authenticated READ authenticated NOTIFY changed)
  public:
    explicit SessionModel(QObject* parent = nullptr);
    QString selected_source() const { return selected_source_; }
    QString selected_name() const;
    QString destination() const { return destination_; }
    QString phase_title() const;
    bool complete() const {
        return transfer_ && transfer_->phase() == bridge::app::TransferPhase::complete;
    }
    bool offered_folder() const {
        return transfer_ && transfer_->manifest() &&
               transfer_->manifest()->kind == bridge::PayloadKind::folder;
    }
    Q_INVOKABLE bool dropUrls(const QList<QUrl>& urls);
    Q_INVOKABLE bool selectSource(const QUrl& url);
    Q_INVOKABLE bool selectDestination(const QUrl& url);
    Q_INVOKABLE void sendSelected(const QString& address, const QString& port, bool resume = false);
    QString status() const { return status_; }
    QString fingerprint() const { return fingerprint_; }
    QString endpoint() const { return endpoint_; }
    QStringList local_addresses() const;
    bool transfer_supported() const { return bridge::app::Transfer::supported(); }
    bool can_confirm() const { return transfer_ && transfer_->can_confirm(); }
    bool busy() const { return transfer_ && transfer_->busy(); }
    bool authenticated() const { return transfer_ && transfer_->authenticated(); }
    bool can_accept() const { return transfer_ && transfer_->can_accept(); }
    bool can_pause() const { return transfer_ && transfer_->can_pause(); }
    bool can_continue() const { return transfer_ && transfer_->can_continue(); }
    QString filename() const;
    QString checkpoint() const;
    double progress() const;
    Q_INVOKABLE void receive(const QString& address, const QString& port,
                             const QString& destination, bool resume = false);
    Q_INVOKABLE void connectPeer(const QString& address, const QString& port, const QString& source,
                                 bool resume = false);
    Q_INVOKABLE void acceptFile();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void continueTransfer();
    Q_INVOKABLE void confirm();
    Q_INVOKABLE void reject();
    Q_INVOKABLE void cancel();
  signals:
    void changed();

  private:
    bool create(bridge::Role role, const QString& address, const QString& port,
                std::uint16_t& parsed_port);
    void show_error(bridge::Error error);
    bridge::Logger logger_;
    std::unique_ptr<bridge::app::Transfer> transfer_;
    QString status_ =
        QStringLiteral("Idle. Choose an interface to receive or a local peer to connect.");
    QString fingerprint_;
    QString endpoint_;
    std::optional<bridge::FileManifest> saved_manifest_;
    QString saved_source_;
    QString selected_source_, destination_;
    bool role_sender_ = false;
};
