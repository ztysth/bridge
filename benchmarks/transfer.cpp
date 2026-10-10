#include "bridge/app/transfer.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <vector>
namespace {
using Clock = std::chrono::steady_clock;
std::filesystem::path native_path(const QString& path) {
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().toStdString());
#endif
}
bool trial(std::uint64_t bytes) {
    QTemporaryDir source, destination;
    if (!source.isValid() || !destination.isValid())
        return false;
    const auto input = native_path(source.path()) / "payload.bin";
    {
        std::ofstream file(input, std::ios::binary);
        std::vector<char> block(bridge::chunk_size);
        std::mt19937 generator(42);
        for (auto& value : block)
            value = static_cast<char>(generator() & 0xffU);
        for (std::uint64_t written = 0; written < bytes; written += block.size())
            file.write(block.data(), static_cast<std::streamsize>(block.size()));
        file.close();
        if (!file)
            return false;
    }
    std::ostringstream logs;
    bridge::Logger logger(logs);
    bridge::app::Transfer sender(bridge::Role::initiator, logger);
    bridge::app::Transfer receiver(bridge::Role::receiver, logger);
    QEventLoop loop;
    QTimer poll, deadline;
    QString sender_fingerprint, receiver_fingerprint;
    int finished = 0;
    bool success = true, streaming = false;
    Clock::time_point stream_start;
    QObject::connect(&sender, &bridge::app::Transfer::pairing_pending, &loop,
                     [&](QString value) { sender_fingerprint = std::move(value); });
    QObject::connect(&receiver, &bridge::app::Transfer::pairing_pending, &loop,
                     [&](QString value) { receiver_fingerprint = std::move(value); });
    const auto complete = [&](bool ok, int) {
        success = success && ok;
        if (++finished == 2)
            loop.quit();
    };
    QObject::connect(&sender, &bridge::app::Transfer::finished, &loop, complete);
    QObject::connect(&receiver, &bridge::app::Transfer::finished, &loop, complete);
    QObject::connect(&sender, &bridge::app::Transfer::changed, &loop, [&] {
        if (!streaming && sender.phase() == bridge::app::TransferPhase::transferring) {
            streaming = true;
            stream_start = Clock::now();
        }
    });
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        // Only this private loopback harness compares/approves identities automatically.
        if (sender.can_confirm() && receiver.can_confirm()) {
            if (sender_fingerprint.isEmpty() || sender_fingerprint != receiver_fingerprint) {
                success = false;
                sender.reject();
                receiver.reject();
                return;
            }
            sender.confirm();
            receiver.confirm();
        }
        if (receiver.can_accept() && !receiver.accept()) {
            success = false;
            sender.cancel();
            receiver.cancel();
        }
    });
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        success = false;
        sender.cancel();
        receiver.cancel();
        loop.quit();
    });
    const auto start = Clock::now();
    auto port = receiver.receive(QHostAddress::LocalHost, 0, native_path(destination.path()));
    if (!port || !sender.send_file(QHostAddress::LocalHost, *port, input))
        return false;
    poll.start(2);
    deadline.start(120000);
    loop.exec();
    const auto end = Clock::now();
    std::error_code error;
    if (!success || finished != 2 || !streaming ||
        std::filesystem::file_size(native_path(destination.path()) / "payload.bin", error) !=
            bytes ||
        error) {
        std::cerr << "benchmark transfer failed\n";
        return false;
    }
    const double total = std::chrono::duration<double>(end - start).count();
    const double data = std::chrono::duration<double>(end - stream_start).count();
    std::cout << "bytes=" << bytes << " total_seconds=" << total << " data_seconds=" << data
              << " total_MBps=" << static_cast<double>(bytes) / total / 1e6
              << " data_MBps=" << static_cast<double>(bytes) / data / 1e6 << '\n';
    return true;
}
} // namespace
int main(int argc, char** argv) {
    std::uint64_t mebibytes = 128;
    unsigned trials = 5;
    if (argc > 3)
        return 2;
    const auto parse = [](const char* argument, auto& value) {
        const std::string_view text(argument);
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    };
    if ((argc > 1 && !parse(argv[1], mebibytes)) || (argc > 2 && !parse(argv[2], trials)) ||
        mebibytes < 1 || mebibytes > 1024 || trials < 1 || trials > 10)
        return 2;
    QCoreApplication application(argc, argv);
    for (unsigned run = 0; run < trials; ++run)
        if (!trial(mebibytes * bridge::chunk_size))
            return 1;
    return 0;
}
