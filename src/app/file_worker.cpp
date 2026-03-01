#include "file_worker.hpp"
#include <QTimer>
namespace bridge::app {
FileWorker::FileWorker() : thread_([this](std::stop_token token) { run(token); }) {}
FileWorker::~FileWorker() {
    stop();
    thread_.join();
}
void FileWorker::stop() {
    thread_.request_stop();
    wake_.notify_all();
}
Result<void> FileWorker::submit(Task task, Completion completion) {
    std::lock_guard lock(mutex_);
    if (thread_.get_stop_token().stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    if (outstanding_)
        return std::unexpected(Error{ErrorCode::queue_full});
    outstanding_ = true;
    job_ = Job{std::move(task), std::move(completion)};
    wake_.notify_one();
    return {};
}
void FileWorker::run(std::stop_token token) {
    {
        State state;
        while (!token.stop_requested()) {
            std::optional<Job> work;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, token, [this] { return job_.has_value(); });
                if (token.stop_requested())
                    break;
                work = std::move(job_);
                job_.reset();
            }
            auto result = work->task(state, token);
            {
                std::lock_guard lock(mutex_);
                outstanding_ = false;
            }
            QTimer::singleShot(
                0, this,
                [callback = std::move(work->completion), value = std::move(result)]() mutable {
                    callback(std::move(value));
                });
        }
    } // Release descriptors before reporting that it is safe to replace the owner.
    QTimer::singleShot(0, this, [this] { emit stopped(); });
}
} // namespace bridge::app
