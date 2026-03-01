#pragma once
#include "bridge/io/checkpoint.hpp"
#include "bridge/io/folder.hpp"
#include <QObject>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>
namespace bridge::app {
// Construct/destroy/submit on the Qt thread. State and all IO handles live only
// on the owned worker. Destruction joins before the QObject context is destroyed.
class FileWorker final : public QObject {
    Q_OBJECT
  public:
    struct State {
        std::optional<io::SourcePayload> source;
        std::optional<io::FolderDestination> folder;
        std::optional<io::PartialFile> partial;
    };
    using Value = std::variant<std::monostate, FileManifest, Chunk, std::uint64_t>;
    using Task = std::function<Result<Value>(State&, std::stop_token)>;
    using Completion = std::function<void(Result<Value>)>;
    FileWorker();
    ~FileWorker() override;
    Result<void> submit(Task task, Completion completion);
    void stop();
  signals:
    void stopped();

  private:
    struct Job {
        Task task;
        Completion completion;
    };
    void run(std::stop_token token);
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<Job> job_;
    bool outstanding_ = false;
    std::jthread thread_;
};
} // namespace bridge::app
