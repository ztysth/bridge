#include "bridge/io/folder.hpp"
#include "bridge/core/diagnostics.hpp"
#include "bridge/security/hash.hpp"
#include <cstdlib>
#include <fstream>
namespace {
struct Root {
    char name[sizeof("/tmp/bridge-folder-fuzz-XXXXXX")] = "/tmp/bridge-folder-fuzz-XXXXXX";
    Root() { BRIDGE_CHECK(::mkdtemp(name) != nullptr); }
    ~Root() {
        std::error_code ec;
        std::filesystem::remove_all(name, ec);
        BRIDGE_CHECK(!ec);
    }
};
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 4096)
        return 0;
    Root root;
    auto digest = bridge::security::sha256(std::span(data, size));
    BRIDGE_CHECK(digest);
    bridge::FileManifest manifest{{1}, "received", size, *digest, bridge::PayloadKind::folder};
    auto folder = bridge::io::FolderDestination::open(root.name, manifest, false);
    BRIDGE_CHECK(folder);
    {
        std::ofstream output(folder->payload_root() / manifest.name, std::ios::binary);
        output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        BRIDGE_CHECK(output.good());
    }
    static_cast<void>(folder->publish());
    return 0;
}
