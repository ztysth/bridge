#include "bridge/security/identity.hpp"
#include <QCoreApplication>
#include <catch2/catch_test_macros.hpp>
TEST_CASE("fresh self-signed identities and fingerprint role binding") {
    int argc = 1;
    char name[] = "bridge-unit";
    char* argv[]{name, nullptr};
    QCoreApplication runtime(argc, argv);
    auto first = bridge::security::make_identity();
    auto second = bridge::security::make_identity();
    REQUIRE(first);
    REQUIRE(second);
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
