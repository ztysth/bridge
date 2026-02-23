#include "bridge/app/session.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QNetworkProxy>
#include <QTimer>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <sstream>
using namespace bridge;
namespace {
struct Runtime {
    int argc = 1;
    char name[19] = "bridge-integration";
    char* argv[2]{name, nullptr};
    QCoreApplication application{argc, argv};
};
struct Pair {
    std::ostringstream logs;
    Logger logger{logs};
    app::Session receiver{Role::receiver, logger}, client{Role::initiator, logger};
    QEventLoop loop;
    QTimer guard;
    QTimer secure_poll;
    QString receiver_fingerprint, client_fingerprint;
    int done = 0;
    bool receiver_ok = false, client_ok = false, guard_expired = false;
    int receiver_error = 0, client_error = 0;
    Pair() {
        QObject::connect(&receiver, &app::Session::pairing_pending, &loop,
                         [this](const QString& value) { receiver_fingerprint = value; });
        QObject::connect(&client, &app::Session::pairing_pending, &loop,
                         [this](const QString& value) { client_fingerprint = value; });
        QObject::connect(&receiver, &app::Session::finished, &loop, [this](bool ok, int code) {
            receiver_ok = ok;
            receiver_error = code;
            if (++done == 2)
                loop.quit();
        });
        QObject::connect(&client, &app::Session::finished, &loop, [this](bool ok, int code) {
            client_ok = ok;
            client_error = code;
            if (++done == 2)
                loop.quit();
        });
        guard.setSingleShot(true);
        QObject::connect(&guard, &QTimer::timeout, &loop, [this] {
            guard_expired = true;
            loop.quit();
        });
    }
    void start() {
        auto port = receiver.receive(QHostAddress::LocalHost, 0);
        REQUIRE(port);
        REQUIRE(receiver.listening());
        REQUIRE(client.connect_peer(QHostAddress::LocalHost, *port));
    }
    void run() {
        guard.start(5000);
        loop.exec();
        REQUIRE_FALSE(guard_expired);
    }
    void once_secure(std::function<void()> callback) {
        // Both screens must be present before the test controller may confirm.
        secure_poll.setInterval(5);
        QObject::connect(&secure_poll, &QTimer::timeout, &loop, [this, callback] {
            if (!receiver_fingerprint.isEmpty() && !client_fingerprint.isEmpty()) {
                secure_poll.stop();
                REQUIRE(receiver_fingerprint == client_fingerprint);
                callback();
            }
        });
        secure_poll.start();
    }
};
} // namespace
TEST_CASE("explicit local receive and bilateral TLS pairing complete cleanly") {
    Runtime runtime;
    Pair pair;
    pair.once_secure([&] {
        REQUIRE_FALSE(pair.receiver.listening());
        REQUIRE_FALSE(pair.client.paired());
        pair.client.confirm();
        pair.receiver.confirm();
    });
    pair.start();
    pair.run();
    REQUIRE(pair.client_ok);
    REQUIRE(pair.receiver_ok);
    REQUIRE(pair.client.phase() == Phase::complete);
    REQUIRE(pair.logs.str().find(pair.client_fingerprint.toStdString()) == std::string::npos);
}
TEST_CASE("rejection cancellation timeout and connection loss do not complete") {
    for (int mode = 0; mode < 4; ++mode) {
        Runtime runtime;
        Pair pair;
        pair.once_secure([&] {
            if (mode == 0)
                pair.receiver.reject();
            else if (mode == 1)
                pair.receiver.cancel();
            else if (mode == 2)
                pair.receiver.diagnostic_transport().expire_for_test();
            else {
                pair.receiver.diagnostic_transport().abort();
                pair.receiver.cancel();
            }
        });
        pair.start();
        pair.run();
        REQUIRE_FALSE(pair.client_ok);
        REQUIRE_FALSE(pair.receiver_ok);
        const auto expected = mode == 0   ? ErrorCode::rejected
                              : mode == 2 ? ErrorCode::timeout
                                          : ErrorCode::cancelled;
        REQUIRE(pair.receiver_error == static_cast<int>(expected));
    }
}
TEST_CASE("one-sided confirmation cannot negotiate; deadline terminates") {
    Runtime runtime;
    Pair pair;
    pair.once_secure([&] {
        pair.client.confirm();
        QTimer::singleShot(30, &pair.loop, [&] {
            REQUIRE_FALSE(pair.receiver.paired());
            REQUIRE_FALSE(pair.client.paired());
            REQUIRE(pair.receiver.phase() == Phase::pairing);
            pair.receiver.diagnostic_transport().expire_for_test();
        });
    });
    pair.start();
    pair.run();
    REQUIRE_FALSE(pair.client_ok);
    REQUIRE_FALSE(pair.receiver_ok);
}
TEST_CASE("hostile live frames rejected before authentication") {
    for (int mode = 0; mode < 7; ++mode) {
        Runtime runtime;
        Pair pair;
        pair.once_secure([&] {
            auto frame = encode_frame(Message::hello, std::array<std::uint8_t, 4>{});
            REQUIRE(frame);
            if (mode == 0) {
                (*frame)[8] = 255;
                (*frame)[9] = 255;
                (*frame)[10] = 255;
                (*frame)[11] = 255;
            }
            if (mode == 1)
                frame->resize(5);
            if (mode == 3)
                (*frame)[5] = 2;
            if (mode == 4)
                (*frame)[6] = 255;
            if (mode == 5)
                (*frame)[7] = 1;
            if (mode == 6) {
                frame = encode_frame(Message::confirm);
                auto duplicate = *frame;
                frame->insert(frame->end(), duplicate.begin(), duplicate.end());
            }
            REQUIRE(pair.client.diagnostic_transport().send_bytes_for_test(*frame));
            if (mode == 1)
                QTimer::singleShot(20, &pair.loop, [&] { pair.client.cancel(); });
        });
        pair.start();
        pair.run();
        REQUIRE_FALSE(pair.receiver_ok);
        const auto expected = mode == 0                ? ErrorCode::oversized_frame
                              : mode == 3              ? ErrorCode::unsupported_version
                              : mode == 2 || mode == 6 ? ErrorCode::invalid_state
                                                       : ErrorCode::malformed_frame;
        REQUIRE(pair.receiver_error == static_cast<int>(expected));
    }
}
TEST_CASE("endpoint policy and receive cancellation") {
    Runtime runtime;
    for (auto address : {"0.0.0.0", "8.8.8.8", "224.0.0.1", "::", "::1", "example.com"})
        REQUIRE_FALSE(net::is_local_address(QHostAddress(address)));
    for (auto address :
         {"127.0.0.1", "10.0.0.1", "172.16.0.1", "172.31.255.254", "192.168.1.2", "169.254.1.1"})
        REQUIRE(net::is_local_address(QHostAddress(address)));
    REQUIRE_FALSE(net::is_local_address(QHostAddress("172.32.0.1")));
    std::ostringstream logs;
    Logger logger(logs);
    app::Session receiver(Role::receiver, logger);
    REQUIRE_FALSE(receiver.receive(QHostAddress::Any, 0));
    REQUIRE(receiver.receive(QHostAddress::LocalHost, 0));
    REQUIRE(receiver.listening());
    receiver.cancel();
    REQUIRE_FALSE(receiver.listening());
}

TEST_CASE("fragmented confirmation is assembled and output is bounded") {
    Runtime runtime;
    Pair pair;
    pair.once_secure([&] {
        auto frame = encode_frame(Message::confirm);
        REQUIRE(frame);
        REQUIRE(pair.client.diagnostic_transport().send_bytes_for_test(
            std::span<const std::uint8_t>(*frame).first(5)));
        QTimer::singleShot(10, &pair.loop,
                           [&, tail = std::vector<std::uint8_t>(frame->begin() + 5, frame->end())] {
                               REQUIRE(pair.receiver.phase() == Phase::pairing);
                               REQUIRE(
                                   pair.client.diagnostic_transport().send_bytes_for_test(tail));
                               REQUIRE(pair.client.diagnostic_transport()
                                           .send_bytes_for_test(std::vector<std::uint8_t>(16385))
                                           .error()
                                           .code == ErrorCode::queue_full);
                               QTimer::singleShot(20, &pair.loop, [&] {
                                   REQUIRE_FALSE(pair.receiver.paired());
                                   pair.receiver.cancel();
                               });
                           });
    });
    pair.start();
    pair.run();
    REQUIRE_FALSE(pair.receiver_ok);
}
TEST_CASE("TLS downgrade and missing client identity are rejected") {
    for (const auto protocol : {QSsl::TlsV1_2, QSsl::TlsV1_3}) {
        Runtime runtime;
        std::ostringstream logs;
        Logger logger(logs);
        app::Session receiver(Role::receiver, logger);
        QEventLoop loop;
        QSslSocket raw;
        bool failed = false, secured = false, expired = false;
        QTimer guard;
        guard.setSingleShot(true);
        QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
            expired = true;
            loop.quit();
        });
        QObject::connect(&receiver, &app::Session::pairing_pending, &loop, [&] { secured = true; });
        QObject::connect(&receiver, &app::Session::finished, &loop, [&](bool ok, int) {
            failed = !ok;
            loop.quit();
        });
        auto port = receiver.receive(QHostAddress::LocalHost, 0);
        REQUIRE(port);
        raw.setProtocol(protocol);
        raw.setPeerVerifyName(QStringLiteral("bridge.local"));
        raw.setProxy(QNetworkProxy::NoProxy);
        QObject::connect(&raw, &QSslSocket::sslErrors, &loop, [&](const QList<QSslError>& errors) {
            if (std::ranges::all_of(errors, [](const QSslError& e) {
                    return e.error() == QSslError::SelfSignedCertificate;
                }))
                raw.ignoreSslErrors(errors);
        });
        raw.connectToHostEncrypted(QStringLiteral("127.0.0.1"), *port,
                                   QStringLiteral("bridge.local"));
        guard.start(2000);
        loop.exec();
        REQUIRE_FALSE(expired);
        REQUIRE(failed);
        REQUIRE_FALSE(secured);
    }
}

TEST_CASE("idle receive deadline closes listener") {
    Runtime runtime;
    std::ostringstream logs;
    Logger logger(logs);
    app::Session receiver(Role::receiver, logger);
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    bool expired = false;
    int code = 0;
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
        expired = true;
        loop.quit();
    });
    QObject::connect(&receiver, &app::Session::finished, &loop, [&](bool, int error) {
        code = error;
        loop.quit();
    });
    REQUIRE(receiver.receive(QHostAddress::LocalHost, 0));
    receiver.diagnostic_transport().set_deadline(20);
    guard.start(2000);
    loop.exec();
    REQUIRE_FALSE(expired);
    REQUIRE(code == static_cast<int>(ErrorCode::timeout));
    REQUIRE_FALSE(receiver.listening());
}
