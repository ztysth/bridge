#include "bridge/security/identity.hpp"
#include <QCoreApplication>
#include <QSslSocket>
#include <catch2/catch_test_macros.hpp>
TEST_CASE("fresh self-signed identities and fingerprint role binding") {
    int argc = 1;
    char name[] = "bridge-unit";
    char* argv[]{name, nullptr};
    QCoreApplication runtime(argc, argv);
    const auto backends = QSslSocket::availableBackends();
    REQUIRE(backends.contains(QStringLiteral("openssl")));
    for (const auto& backend : backends) {
        if (backend != QStringLiteral("openssl")) {
            REQUIRE(QSslSocket::setActiveBackend(backend));
            break;
        }
    }
    auto first = bridge::security::make_identity();
    auto second = bridge::security::make_identity();
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(QSslSocket::activeBackend() == QStringLiteral("openssl"));
    REQUIRE(bridge::security::validate_peer(first->certificate));
    REQUIRE(first->certificate.toDer() != second->certificate.toDer());
    auto pair = bridge::security::pairing_fingerprint(first->certificate, second->certificate);
    REQUIRE(pair);
    REQUIRE(pair->size() == 64);
    REQUIRE(*pair !=
            *bridge::security::pairing_fingerprint(second->certificate, first->certificate));
    REQUIRE_FALSE(bridge::security::validate_peer(QSslCertificate{}));
    auto bad = first->certificate.toDer();
    bad[bad.size() - 1] ^= 1;
    REQUIRE_FALSE(bridge::security::validate_peer(QSslCertificate(bad, QSsl::Der)));
}
