#pragma once
#include "bridge/core/error.hpp"
#include <QSslCertificate>
#include <QSslKey>
#include <QString>
namespace bridge::security {
struct Identity {
    QSslCertificate certificate;
    QSslKey key;
};
Result<Identity> make_identity();
Result<void> validate_peer(const QSslCertificate& certificate);
Result<QString> pairing_fingerprint(const QSslCertificate& client, const QSslCertificate& server);
} // namespace bridge::security
