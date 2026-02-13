#include "bridge/io/checkpoint.hpp"
#include "bridge/core/diagnostics.hpp"
#include "bridge/security/hash.hpp"
#include <cstdlib>
#include <fstream>
namespace {
struct Root {
    char name[sizeof("/tmp/bridge-checkpoint-fuzz-XXXXXX")] = "/tmp/bridge-checkpoint-fuzz-XXXXXX";
    Root() { BRIDGE_CHECK(::mkdtemp(name) != nullptr); }
    ~Root() {
        std::error_code error;
        std::filesystem::remove_all(name, error);
        BRIDGE_CHECK(!error);
    }
};
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 4096)
        return 0;
    Root root;
    const std::array<std::uint8_t, 64> content{};
    auto digest = bridge::security::sha256(content);
    BRIDGE_CHECK(digest);
    bridge::FileManifest manifest{{1}, "received.bin", content.size(), *digest};
    {
        auto file = bridge::io::PartialFile::create(root.name, manifest);
        BRIDGE_CHECK(file);
        BRIDGE_CHECK(file->append(content, *digest));
    }
    std::filesystem::path journal;
    for (const auto& entry : std::filesystem::directory_iterator(root.name))
        if (entry.path().extension() == ".journal")
            journal = entry.path();
    BRIDGE_CHECK(!journal.empty());
    {
        std::ofstream stream(journal, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        BRIDGE_CHECK(stream.good());
    }
    // Exercise actual bounded recovery through private temporary files.
    auto resumed = bridge::io::PartialFile::resume(root.name, manifest);
    if (resumed && resumed->durable_bytes() == content.size())
        BRIDGE_CHECK(resumed->finish());
    return 0;
}
