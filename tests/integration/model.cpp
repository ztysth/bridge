#include "model.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
TEST_CASE("desktop model Pause Continue commands resume actual bytes and preserve receiver hold") {
    int argc = 1;
    char name[15] = "bridge-model";
    char* argv[2]{name, nullptr};
    QCoreApplication application(argc, argv);
    SessionModel receiver, sender;
    if (!receiver.transfer_supported()) {
        REQUIRE_FALSE(receiver.can_pause());
        return;
    }
    QTemporaryDir src(QDir::tempPath() + QStringLiteral("/bridge-src-\u6d4b\u8bd5-XXXXXX"));
    QTemporaryDir root(QDir::tempPath() + QStringLiteral("/bridge-dst-\u6d4b\u8bd5-XXXXXX"));
    REQUIRE(src.isValid());
    REQUIRE(root.isValid());
    const auto path = src.path() + "/ui.bin";
    {
        QFile file(path);
        REQUIRE(file.open(QIODevice::WriteOnly));
        const QByteArray block(bridge::chunk_size, 'z');
        for (int i = 0; i < 4; ++i)
            REQUIRE(file.write(block) == block.size());
    }
    QEventLoop loop;
    QTimer guard, poll;
    bool expired = false, requested = false, checked = false;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
        expired = true;
        loop.quit();
    });
    receiver.receive("127.0.0.1", "0", root.path());
    REQUIRE(receiver.busy());
    sender.connectPeer("127.0.0.1", receiver.endpoint().section(':', 1), path);
    REQUIRE(sender.busy());
    poll.setInterval(2);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (sender.can_confirm() && receiver.can_confirm()) {
            REQUIRE(sender.fingerprint() == receiver.fingerprint());
            sender.confirm();
            receiver.confirm();
        }
        if (receiver.can_accept()) {
            REQUIRE(receiver.filename() == "ui.bin");
            receiver.acceptFile();
        }
        if (!sender.busy() && !receiver.busy())
            loop.quit();
    });
    QObject::connect(&sender, &SessionModel::changed, &loop, [&] {
        if (!requested && sender.can_pause() && sender.progress() > 0 && sender.progress() < 1) {
            requested = true;
            sender.pause();
            receiver.pause();
        }
        if (requested && !checked && sender.can_continue() && receiver.can_continue()) {
            checked = true;
            const auto checkpoint = sender.checkpoint();
            QTimer::singleShot(80, &loop, [&, checkpoint] {
                REQUIRE(sender.checkpoint() == checkpoint);
                REQUIRE_FALSE(sender.can_pause());
                sender.continueTransfer();
                QTimer::singleShot(80, &loop, [&, checkpoint] {
                    REQUIRE(sender.checkpoint() == checkpoint);
                    REQUIRE(receiver.can_continue());
                    receiver.continueTransfer();
                });
            });
        }
    });
    guard.start(10000);
    poll.start();
    loop.exec();
    REQUIRE_FALSE(expired);
    REQUIRE(requested);
    REQUIRE(checked);
    REQUIRE(sender.authenticated());
    REQUIRE(receiver.authenticated());
    REQUIRE(sender.progress() == 1);
    REQUIRE(receiver.progress() == 1);
    REQUIRE(sender.status().contains("complete"));
    QFile received(root.path() + "/ui.bin");
    REQUIRE(received.size() == 4 * bridge::chunk_size);
}
TEST_CASE("desktop accepts one local folder drop and transmits its tree after consent") {
    int argc = 1;
    char name[15] = "bridge-model";
    char* argv[2]{name, nullptr};
    QCoreApplication application(argc, argv);
    SessionModel sender, receiver;
    QTemporaryDir src(QDir::tempPath() + QStringLiteral("/bridge-src-\u6d4b\u8bd5-XXXXXX"));
    QTemporaryDir root(QDir::tempPath() + QStringLiteral("/bridge-dst-\u6d4b\u8bd5-XXXXXX"));
    REQUIRE(src.isValid());
    REQUIRE(root.isValid());
    const auto folder = src.path() + "/DropMe";
    REQUIRE(QDir().mkpath(folder + "/nested/empty"));
    {
        QFile file(folder + "/nested/hello.txt");
        REQUIRE(file.open(QIODevice::WriteOnly));
        REQUIRE(file.write("hello", 5) == 5);
    }
    REQUIRE_FALSE(sender.dropUrls({}));
    REQUIRE_FALSE(sender.dropUrls({QUrl("https://example.com/private")}));
    REQUIRE_FALSE(sender.dropUrls({QUrl("file://remote/share")}));
    REQUIRE_FALSE(sender.dropUrls({QUrl("file://localhost/tmp/share")}));
    REQUIRE_FALSE(sender.dropUrls({QUrl("file:///tmp/file?query")}));
    const auto url = QUrl::fromLocalFile(folder);
    REQUIRE_FALSE(sender.dropUrls({url, url}));
    REQUIRE(sender.dropUrls({url}));
    REQUIRE(sender.selected_name() == "DropMe");
    REQUIRE_FALSE(sender.busy());
    REQUIRE(receiver.selectDestination(QUrl::fromLocalFile(root.path())));
    if (!sender.transfer_supported())
        return;
    receiver.receive("127.0.0.1", "0", receiver.destination());
    sender.sendSelected("127.0.0.1", receiver.endpoint().section(':', 1));
    REQUIRE_FALSE(sender.dropUrls({url}));
    QEventLoop loop;
    QTimer poll, guard;
    bool expired = false, consent = false;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (sender.can_confirm() && receiver.can_confirm()) {
            REQUIRE(sender.fingerprint() == receiver.fingerprint());
            sender.confirm();
            receiver.confirm();
        }
        if (receiver.can_accept()) {
            REQUIRE(receiver.offered_folder());
            REQUIRE(receiver.filename() == "DropMe");
            consent = true;
            receiver.acceptFile();
        }
        if (!sender.busy() && !receiver.busy())
            loop.quit();
    });
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
        expired = true;
        loop.quit();
    });
    guard.start(10000);
    poll.start(2);
    loop.exec();
    REQUIRE_FALSE(expired);
    REQUIRE(consent);
    REQUIRE(sender.complete());
    REQUIRE(receiver.complete());
    REQUIRE(QDir(root.path() + "/DropMe/nested/empty").exists());
    QFile received(root.path() + "/DropMe/nested/hello.txt");
    REQUIRE(received.open(QIODevice::ReadOnly));
    REQUIRE(received.readAll() == "hello");
}
