#include "bridge/io/checkpoint.hpp"
#include "bridge/security/hash.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>
using namespace bridge;
int main(int argc, char** argv) {
    if (argc != 3)
        return 2;
    const std::string_view action(argv[1]);
    io::CheckpointFault point = io::CheckpointFault::none;
    if (action == "crash-data")
        point = io::CheckpointFault::after_data_sync;
    else if (action == "crash-torn")
        point = io::CheckpointFault::torn_record;
    else if (action == "crash-record")
        point = io::CheckpointFault::after_record_sync;
    else if (action == "crash-publish")
        point = io::CheckpointFault::after_publish;
    else if (action != "crash" && action != "resume")
        return 2;
    const std::vector<std::uint8_t> first(chunk_size, 23);
    const std::array<std::uint8_t, 31> last{7, 9, 11};
    auto hash = security::Sha256::create();
    if (!hash || !hash->update(first) || !hash->update(last))
        return 3;
    auto full = hash->finish();
    auto first_hash = security::sha256(first);
    auto last_hash = security::sha256(last);
    if (!full || !first_hash || !last_hash)
        return 3;
    FileManifest manifest{{1}, "received.bin", chunk_size + last.size(), *full};
    auto file = action == "resume" ? io::PartialFile::resume(argv[2], manifest)
                                   : io::PartialFile::create(argv[2], manifest, {point, 1});
    if (!file)
        return 4;
    if (action != "resume") {
        auto result = file->append(first, *first_hash);
        if (point == io::CheckpointFault::after_publish) {
            if (!result || !file->append(last, *last_hash))
                return 5;
            auto published = file->finish();
            if (published || published.error().code != ErrorCode::injected_failure)
                return 5;
        } else if ((point == io::CheckpointFault::none && !result) ||
                   (point != io::CheckpointFault::none &&
                    (result || result.error().code != ErrorCode::injected_failure)))
            return 5;
        // Test-only hard exit deliberately bypasses C++ destructors and lock release.
        // OS teardown releases handles; the next executable must validate disk state.
        std::_Exit(70);
    }
    std::cout << "resumed_bytes=" << file->durable_bytes() << '\n';
    if (!file->committed()) {
        if (file->durable_bytes() == 0 && !file->append(first, *first_hash))
            return 5;
        if (file->durable_bytes() == chunk_size && !file->append(last, *last_hash))
            return 5;
        if (!file->finish())
            return 5;
    }
    return file->committed() ? 0 : 6;
}
