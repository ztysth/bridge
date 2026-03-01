#include "file_worker.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
using namespace bridge;
TEST_CASE(
    "one scoped worker rejects queued jobs and stops cooperatively without blocking event loop") {
    int argc = 1;
    char name[15] = "bridge-worker";
    char* argv[2]{name, nullptr};
    QCoreApplication application(argc, argv);
    app::FileWorker worker;
    QEventLoop loop;
    QTimer guard, tick;
    std::atomic<bool> started = false;
    bool responsive = false, expired = false, stopped = false;
    REQUIRE(worker.submit(
        [&](app::FileWorker::State&, std::stop_token token) -> Result<app::FileWorker::Value> {
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(mutex);
            started.store(true);
            wake.wait(lock, token, [] { return false; });
            return std::unexpected(Error{ErrorCode::cancelled});
        },
        [](auto) {}));
    REQUIRE(
        worker
            .submit([](auto&, auto) -> Result<app::FileWorker::Value> { return std::monostate{}; },
                    [](auto) {})
            .error()
            .code == ErrorCode::queue_full);
    tick.setInterval(2);
    QObject::connect(&tick, &QTimer::timeout, &loop, [&] {
        if (started.load()) {
            responsive = true;
            worker.stop();
            tick.stop();
        }
    });
    QObject::connect(&worker, &app::FileWorker::stopped, &loop, [&] {
        stopped = true;
        loop.quit();
    });
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
        expired = true;
        worker.stop();
        loop.quit();
    });
    guard.start(2000);
    tick.start();
    loop.exec();
    REQUIRE_FALSE(expired);
    REQUIRE(responsive);
    REQUIRE(stopped);
    REQUIRE_FALSE(worker.submit(
        [](auto&, auto) -> Result<app::FileWorker::Value> { return std::monostate{}; },
        [](auto) {}));
}
