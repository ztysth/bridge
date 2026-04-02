#include <condition_variable>
#include <expected>
#include <mutex>
#include <stop_token>
#include <thread>

// Compile/link in CMake; execute on native CI before building the dependency SDK.
int main() {
    std::expected<int, int> value = 42;
    std::stop_source source;
    std::stop_callback callback(source.get_token(), [] {});
    source.request_stop();
    std::jthread worker([](std::stop_token token) {
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        wake.wait(lock, token, [] { return false; });
    });
    worker.request_stop();
    return value.value() == 42 && source.stop_requested() ? 0 : 1;
}
