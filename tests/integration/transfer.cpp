#include "bridge/app/transfer.hpp"
#include "bridge/io/source.hpp"
#include "bridge/security/hash.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>
using namespace bridge;
namespace {
struct Runtime {
    int argc = 1;
    char name[16] = "bridge-transfer";
    char* argv[2]{name, nullptr};
    QCoreApplication application{argc, argv};
};
void write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file);
}
struct Pair {
    std::ostringstream logs;
    Logger logger{logs};
    app::Transfer sender{Role::initiator, logger}, receiver{Role::receiver, logger};
    QEventLoop loop;
    QTimer guard, poll;
    QString a, b;
    int done = 0;
    bool success_a = false, success_b = false, expired = false;
    int error_a = 0, error_b = 0;
    Pair() {
        QObject::connect(&sender, &app::Transfer::pairing_pending, &loop,
                         [this](QString f) { a = std::move(f); });
        QObject::connect(&receiver, &app::Transfer::pairing_pending, &loop,
                         [this](QString f) { b = std::move(f); });
        QObject::connect(&sender, &app::Transfer::finished, &loop, [this](bool ok, int code) {
            success_a = ok;
            error_a = code;
            if (++done == 2)
                loop.quit();
        });
        QObject::connect(&receiver, &app::Transfer::finished, &loop, [this](bool ok, int code) {
            success_b = ok;
            error_b = code;
            if (++done == 2)
                loop.quit();
        });
        guard.setSingleShot(true);
        QObject::connect(&guard, &QTimer::timeout, &loop, [this] {
            expired = true;
            loop.quit();
        });
        poll.setInterval(2);
        QObject::connect(&poll, &QTimer::timeout, &loop, [this] {
            if (sender.can_confirm() && receiver.can_confirm()) {
                REQUIRE(a == b);
                sender.confirm();
                receiver.confirm();
            }
            if (receiver.can_accept())
                REQUIRE(receiver.accept());
        });
    }
    void start(const std::filesystem::path& source, const std::filesystem::path& root,
               app::ReceiveMode mode = app::ReceiveMode::create,
               std::optional<FileManifest> resume = {}) {
        auto port = receiver.receive(QHostAddress::LocalHost, 0, root, mode);
        REQUIRE(port);
        const auto result =
            sender.send_file(QHostAddress::LocalHost, *port, source, std::move(resume));
        REQUIRE(result);
        poll.start();
    }
    void run() {
        guard.start(10000);
        loop.exec();
        CAPTURE(sender.status().toStdString(), receiver.status().toStdString(), done);
        REQUIRE_FALSE(expired);
    }
};
} // namespace
TEST_CASE("single-file transfer pause barriers preserve bytes until both users continue") {
    if (!app::Transfer::supported())
        return;
    for (int mode = 0; mode < 3; ++mode) {
        Runtime runtime;
        QTemporaryDir source_dir, root;
        REQUIRE(source_dir.isValid());
        REQUIRE(root.isValid());
        auto source = std::filesystem::path(source_dir.path().toStdString()) / "payload.bin";
        std::vector<std::uint8_t> data(4 * chunk_size + 17, 73);
        write_file(source, data);
        Pair pair;
        bool requested = false, checked = false;
        QObject::connect(&pair.sender, &app::Transfer::changed, &pair.loop, [&] {
            if (!requested && pair.sender.durable_bytes() == chunk_size &&
                pair.sender.can_pause()) {
                requested = true;
                if (mode != 1)
                    REQUIRE(pair.sender.pause());
                if (mode != 0)
                    REQUIRE(pair.receiver.pause());
            }
            if (requested && !checked && pair.sender.phase() == app::TransferPhase::paused &&
                pair.receiver.phase() == app::TransferPhase::paused) {
                checked = true;
                const auto bytes = pair.sender.durable_bytes();
                REQUIRE(bytes == pair.receiver.durable_bytes());
                QTimer::singleShot(100, &pair.loop, [&, bytes] {
                    REQUIRE(pair.sender.durable_bytes() == bytes);
                    REQUIRE(pair.receiver.durable_bytes() == bytes);
                    REQUIRE_FALSE(std::filesystem::exists(
                        std::filesystem::path(root.path().toStdString()) / "payload.bin"));
                    if (mode == 0)
                        REQUIRE(pair.sender.continue_transfer());
                    else if (mode == 1)
                        REQUIRE(pair.receiver.continue_transfer());
                    else {
                        REQUIRE(pair.sender.continue_transfer());
                        REQUIRE(pair.sender.peer_paused());
                        QTimer::singleShot(100, &pair.loop, [&, bytes] {
                            REQUIRE(pair.sender.durable_bytes() == bytes);
                            REQUIRE(pair.receiver.durable_bytes() == bytes);
                            REQUIRE(pair.receiver.continue_transfer());
                        });
                    }
                });
            }
        });
        pair.start(source, root.path().toStdString());
        pair.run();
        REQUIRE(requested);
        REQUIRE(checked);
        REQUIRE(pair.success_a);
        REQUIRE(pair.success_b);
        auto final = io::SourceFile::open(
            std::filesystem::path(root.path().toStdString()) / "payload.bin", {1});
        REQUIRE(final);
        REQUIRE(final->manifest().digest == *security::sha256(data));
        REQUIRE(pair.logs.str().find("payload.bin") == std::string::npos);
    }
}
TEST_CASE("cancel paused transfer then freshly paired resume starts at verified checkpoint") {
    if (!app::Transfer::supported())
        return;
    Runtime runtime;
    QTemporaryDir src, root;
    auto path = std::filesystem::path(src.path().toStdString()) / "resume.bin";
    std::vector<std::uint8_t> data(3 * chunk_size + 5, 22);
    write_file(path, data);
    std::optional<FileManifest> saved;
    {
        Pair p;
        bool requested = false, cancelled = false;
        QObject::connect(&p.sender, &app::Transfer::changed, &p.loop, [&] {
            if (!requested && p.sender.durable_bytes() == chunk_size && p.sender.can_pause()) {
                requested = true;
                REQUIRE(p.sender.pause());
            }
            if (!cancelled && p.sender.phase() == app::TransferPhase::paused) {
                cancelled = true;
                saved = p.sender.manifest();
                p.sender.cancel();
            }
        });
        p.start(path, root.path().toStdString());
        p.run();
        REQUIRE_FALSE(p.success_a);
        REQUIRE_FALSE(p.success_b);
        REQUIRE(cancelled);
        REQUIRE(p.error_a == static_cast<int>(ErrorCode::cancelled));
    }
    Pair p;
    bool saw_prefix = false;
    QObject::connect(&p.sender, &app::Transfer::changed, &p.loop, [&] {
        if (p.sender.durable_bytes() == chunk_size)
            saw_prefix = true;
    });
    p.start(path, root.path().toStdString(), app::ReceiveMode::resume, saved);
    p.run();
    REQUIRE(saw_prefix);
    REQUIRE(p.success_a);
    REQUIRE(p.success_b);
}
TEST_CASE("empty files, disk failures and destination conflicts are real transfer outcomes") {
    if (!app::Transfer::supported())
        return;
    for (int mode = 0; mode < 3; ++mode) {
        Runtime runtime;
        QTemporaryDir src, root;
        auto path = std::filesystem::path(src.path().toStdString()) / "file.bin";
        write_file(path, std::vector<std::uint8_t>(mode == 0 ? 0 : 7, 1));
        Pair p;
        if (mode == 1)
            p.receiver.diagnostic_injection({io::CheckpointFault::disk_full});
        if (mode == 2)
            write_file(std::filesystem::path(root.path().toStdString()) / "file.bin", {9});
        p.start(path, root.path().toStdString());
        p.run();
        REQUIRE(p.success_a == (mode == 0));
        REQUIRE(p.success_b == (mode == 0));
        if (mode > 0)
            REQUIRE(p.error_b == static_cast<int>(mode == 1 ? ErrorCode::disk_full
                                                            : ErrorCode::destination_conflict));
    }
}
TEST_CASE("hostile authenticated file frames never publish data") {
    if (!app::Transfer::supported())
        return;
    for (int mode = 0; mode < 9; ++mode) {
        Runtime runtime;
        QTemporaryDir root;
        std::ostringstream logs;
        Logger logger(logs);
        app::Transfer receiver(Role::receiver, logger);
        app::Session client(Role::initiator, logger, nullptr, SessionPurpose::file);
        QEventLoop loop;
        QTimer poll, guard;
        QString a, b;
        bool expired = false, injected = false, failed = false;
        int code = 0;
        FileManifest m{{1},
                       mode == 1 ? "../escape" : "hostile.bin",
                       3,
                       *security::sha256(std::array<std::uint8_t, 3>{1, 2, 3})};
        QObject::connect(&client, &app::Session::pairing_pending, &loop,
                         [&](QString f) { a = std::move(f); });
        QObject::connect(&receiver, &app::Transfer::pairing_pending, &loop,
                         [&](QString f) { b = std::move(f); });
        QObject::connect(&receiver, &app::Transfer::finished, &loop, [&](bool ok, int error) {
            failed = !ok;
            code = error;
            loop.quit();
        });
        QObject::connect(&client, &app::Session::ready, &loop, [&] {
            REQUIRE(client.send_application(Message::offer, encode_offer(m)));
            if (mode == 2)
                REQUIRE(client.send_application(Message::data_chunk,
                                                encode_chunk({m.id, 0, m.digest, {1, 2, 3}})));
        });
        QObject::connect(&receiver, &app::Transfer::changed, &loop, [&] {
            if (receiver.can_accept() && mode >= 3)
                REQUIRE(receiver.accept());
            if (!injected && receiver.phase() == app::TransferPhase::transferring) {
                injected = true;
                if (mode == 3)
                    REQUIRE(client.send_application(Message::data_chunk,
                                                    encode_chunk({m.id, 0, {}, {1, 2, 3}})));
                if (mode == 4) {
                    auto chunk = encode_chunk({m.id, 0, m.digest, {1, 2, 3}});
                    REQUIRE(client.send_application(Message::data_chunk, chunk));
                    REQUIRE(client.send_application(Message::data_chunk, chunk));
                }
                if (mode == 5)
                    REQUIRE(client.send_application(Message::finish_file, m.id));
                if (mode == 6)
                    REQUIRE(client.send_application(Message::hold, encode_hold({99, true})));
                if (mode == 7) {
                    REQUIRE(client.send_application(Message::hold, encode_hold({1, true})));
                    REQUIRE(client.send_application(Message::pause_barrier,
                                                    encode_barrier({1, 0, chunk_size})));
                }
                if (mode == 8) {
                    auto header = *encode_frame(Message::confirm);
                    header[6] = 16;
                    REQUIRE(client.diagnostic_transport().send_bytes_for_test(header));
                }
            }
        });
        poll.setInterval(2);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (client.can_confirm() && receiver.can_confirm()) {
                REQUIRE(a == b);
                poll.stop();
                if (mode == 0) {
                    auto header = *encode_frame(Message::data_chunk,
                                                std::vector<std::uint8_t>(max_file_payload));
                    header.resize(header_size);
                    REQUIRE(client.diagnostic_transport().send_bytes_for_test(header));
                } else {
                    client.confirm();
                    receiver.confirm();
                }
            }
        });
        guard.setSingleShot(true);
        QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
            expired = true;
            receiver.cancel();
            loop.quit();
        });
        auto port = receiver.receive(QHostAddress::LocalHost, 0, root.path().toStdString());
        REQUIRE(port);
        REQUIRE(client.connect_peer(QHostAddress::LocalHost, *port));
        poll.start();
        guard.start(5000);
        loop.exec();
        CAPTURE(mode, code);
        REQUIRE_FALSE(expired);
        REQUIRE(failed);
        const auto expected = mode == 1   ? ErrorCode::invalid_path
                              : mode == 3 ? ErrorCode::checksum_mismatch
                              : mode == 8 ? ErrorCode::malformed_frame
                                          : ErrorCode::invalid_state;
        REQUIRE(code == static_cast<int>(expected));
        REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path(root.path().toStdString()) /
                                              "hostile.bin"));
        REQUIRE_FALSE(std::filesystem::exists(
            std::filesystem::path(root.path().toStdString()).parent_path() / "escape"));
    }
}
TEST_CASE("source changes after a pause fail instead of silently mixing file versions") {
    if (!app::Transfer::supported())
        return;
    Runtime runtime;
    QTemporaryDir src, root;
    auto path = std::filesystem::path(src.path().toStdString()) / "changed.bin";
    write_file(path, std::vector<std::uint8_t>(2 * chunk_size, 8));
    Pair p;
    bool requested = false, mutated = false;
    QObject::connect(&p.sender, &app::Transfer::changed, &p.loop, [&] {
        if (!requested && p.sender.durable_bytes() == chunk_size && p.sender.can_pause()) {
            requested = true;
            REQUIRE(p.sender.pause());
        }
        if (!mutated && p.sender.can_continue()) {
            mutated = true;
            {
                std::ofstream file(path, std::ios::app);
                file << "x";
            }
            REQUIRE(p.sender.continue_transfer());
        }
    });
    p.start(path, root.path().toStdString());
    p.run();
    REQUIRE(mutated);
    REQUIRE_FALSE(p.success_a);
    REQUIRE_FALSE(p.success_b);
    REQUIRE(p.error_a == static_cast<int>(ErrorCode::source_changed));
}
TEST_CASE("authenticated heartbeats keep a deliberate pause alive") {
    if (!app::Transfer::supported())
        return;
    Runtime runtime;
    QTemporaryDir src, root;
    auto path = std::filesystem::path(src.path().toStdString()) / "heartbeat.bin";
    write_file(path, std::vector<std::uint8_t>(chunk_size + 1, 9));
    Pair p;
    bool requested = false, waited = false;
    QObject::connect(&p.sender, &app::Transfer::changed, &p.loop, [&] {
        if (!requested && p.sender.durable_bytes() == chunk_size && p.sender.can_pause()) {
            requested = true;
            REQUIRE(p.sender.pause());
        }
        if (!waited && p.sender.can_continue()) {
            waited = true;
            p.sender.diagnostic_session().diagnostic_transport().set_deadline(3000);
            p.receiver.diagnostic_session().diagnostic_transport().set_deadline(3000);
            QTimer::singleShot(4100, &p.loop, [&] {
                REQUIRE(p.sender.can_continue());
                REQUIRE(p.sender.durable_bytes() == chunk_size);
                REQUIRE(p.receiver.phase() == app::TransferPhase::paused);
                REQUIRE(p.sender.continue_transfer());
            });
        }
    });
    p.start(path, root.path().toStdString());
    p.run();
    REQUIRE(waited);
    REQUIRE(p.success_a);
    REQUIRE(p.success_b);
}
TEST_CASE("sender rejects unsolicited durable acknowledgements") {
    if (!app::Transfer::supported())
        return;
    Runtime runtime;
    QTemporaryDir src;
    auto path = std::filesystem::path(src.path().toStdString()) / "ack.bin";
    write_file(path, {1, 2, 3});
    std::ostringstream logs;
    Logger logger(logs);
    app::Transfer sender(Role::initiator, logger);
    app::Session peer(Role::receiver, logger, nullptr, SessionPurpose::file);
    QEventLoop loop;
    QTimer poll, guard;
    QString a, b;
    bool expired = false;
    int code = 0;
    QObject::connect(&sender, &app::Transfer::pairing_pending, &loop,
                     [&](QString f) { a = std::move(f); });
    QObject::connect(&peer, &app::Session::pairing_pending, &loop,
                     [&](QString f) { b = std::move(f); });
    QObject::connect(&peer, &app::Session::application_frame, &loop, [&](const Frame& f) {
        if (f.type == Message::offer) {
            auto m = decode_offer(f.payload);
            REQUIRE(m);
            REQUIRE(peer.send_application(Message::durable_ack, encode_prefix({m->id, 3})));
        }
    });
    QObject::connect(&sender, &app::Transfer::finished, &loop, [&](bool ok, int error) {
        REQUIRE_FALSE(ok);
        code = error;
        loop.quit();
    });
    poll.setInterval(2);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (sender.can_confirm() && peer.can_confirm()) {
            REQUIRE(a == b);
            poll.stop();
            sender.confirm();
            peer.confirm();
        }
    });
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
        expired = true;
        sender.cancel();
        loop.quit();
    });
    auto port = peer.receive(QHostAddress::LocalHost, 0);
    REQUIRE(port);
    REQUIRE(sender.send_file(QHostAddress::LocalHost, *port, path));
    poll.start();
    guard.start(5000);
    loop.exec();
    REQUIRE_FALSE(expired);
    REQUIRE(code == static_cast<int>(ErrorCode::invalid_state));
    REQUIRE(sender.durable_bytes() == 0);
}
TEST_CASE("folder TLS transfer pauses cancels resumes and publishes its complete tree") {
    if (!app::Transfer::supported())
        return;
    Runtime runtime;
    QTemporaryDir src, root;
    auto folder = std::filesystem::path(src.path().toStdString()) / "Photos";
    std::filesystem::create_directories(folder / "nested" / "empty");
    auto data = std::vector<std::uint8_t>(3 * chunk_size + 5, 83);
    write_file(folder / "nested" / "image.bin", data);
    write_file(folder / "zero.bin", {});
    std::optional<FileManifest> saved;
    {
        Pair p;
        bool paused = false, cancelled = false;
        QObject::connect(&p.sender, &app::Transfer::changed, &p.loop, [&] {
            if (!paused && p.sender.can_pause() && p.sender.durable_bytes() >= chunk_size) {
                paused = true;
                REQUIRE(p.sender.pause());
            }
            if (paused && !cancelled && p.sender.can_continue() &&
                p.receiver.phase() == app::TransferPhase::paused) {
                cancelled = true;
                saved = p.sender.manifest();
                REQUIRE(saved->kind == PayloadKind::folder);
                REQUIRE_FALSE(std::filesystem::exists(
                    std::filesystem::path(root.path().toStdString()) / "Photos"));
                p.sender.cancel();
            }
        });
        p.start(folder, root.path().toStdString());
        p.run();
        REQUIRE(cancelled);
        REQUIRE_FALSE(p.success_a);
        REQUIRE_FALSE(p.success_b);
    }
    {
        Pair p;
        bool continued = false, paused = false;
        QObject::connect(&p.sender, &app::Transfer::changed, &p.loop, [&] {
            if (!paused && p.sender.can_pause()) {
                paused = true;
                REQUIRE(p.sender.pause());
            }
            if (paused && !continued && p.sender.can_continue()) {
                continued = true;
                REQUIRE(p.sender.continue_transfer());
            }
        });
        p.start(folder, root.path().toStdString(), app::ReceiveMode::resume, saved);
        p.run();
        REQUIRE(p.success_a);
        REQUIRE(p.success_b);
        REQUIRE(continued);
        REQUIRE(p.logs.str().find("Photos") == std::string::npos);
    }
    auto received = std::filesystem::path(root.path().toStdString()) / "Photos";
    REQUIRE(std::filesystem::is_directory(received / "nested" / "empty"));
    REQUIRE(std::filesystem::file_size(received / "zero.bin") == 0);
    auto final = io::SourceFile::open(received / "nested" / "image.bin", {9});
    REQUIRE(final);
    REQUIRE(final->manifest().digest == *security::sha256(data));
    Pair conflict;
    conflict.start(folder, root.path().toStdString());
    conflict.run();
    REQUIRE_FALSE(conflict.success_b);
    REQUIRE(conflict.error_b == static_cast<int>(ErrorCode::destination_conflict));
}
TEST_CASE("empty folder transfer preserves its root directory") {
    if (!app::Transfer::supported())
        return;
    Runtime runtime;
    QTemporaryDir src, root;
    auto folder = std::filesystem::path(src.path().toStdString()) / "Empty";
    std::filesystem::create_directory(folder);
    Pair p;
    p.start(folder, root.path().toStdString());
    p.run();
    REQUIRE(p.success_a);
    REQUIRE(p.success_b);
    REQUIRE(std::filesystem::is_empty(std::filesystem::path(root.path().toStdString()) / "Empty"));
}
