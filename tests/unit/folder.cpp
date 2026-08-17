#include "bridge/io/folder.hpp"
#include "bridge/io/checkpoint.hpp"
#include "bridge/security/hash.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include "../windows_junction.hpp"
using namespace bridge;
namespace {
struct Fixture {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("bridge-folders-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() {
        std::filesystem::create_directories(root / "source" / "nested" / "empty");
        std::filesystem::create_directory(root / "dest");
    }
    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};
void write(const std::filesystem::path& path, std::string_view data) {
    std::ofstream file(path, std::ios::binary);
    REQUIRE(file);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    REQUIRE(file);
}
void record(std::vector<std::uint8_t>& data, std::uint8_t kind, std::string_view path,
            std::uint64_t size = 0) {
    data.push_back(kind);
    data.push_back(static_cast<std::uint8_t>(path.size() >> 8U));
    data.push_back(static_cast<std::uint8_t>(path.size()));
    for (unsigned i = 0; i < 8; ++i)
        data.push_back(static_cast<std::uint8_t>(size >> ((7U - i) * 8U)));
    data.insert(data.end(), path.begin(), path.end());
}
std::vector<std::uint8_t> magic() { return {'B', 'R', 'F', 'O', 'L', 'D', '0', '1'}; }
void receive(io::SourcePayload& source, io::PartialFile& partial) {
    for (auto offset = partial.durable_bytes(); offset < source.manifest().size;) {
        auto chunk = source.read(offset);
        REQUIRE(chunk);
        auto appended = partial.append(chunk->data, chunk->digest);
        REQUIRE(appended);
        offset = *appended;
    }
    auto finished = partial.finish();
    if (!finished) {
        CAPTURE(static_cast<unsigned>(finished.error().code), finished.error().native_code);
        REQUIRE(finished);
    }
}
} // namespace
TEST_CASE("folder bundles preserve nested empty directories and frozen file contents") {
    if (!io::supports_file_io()) {
        REQUIRE_FALSE(io::SourcePayload::open("unsupported", {1}));
        return;
    }
    Fixture f;
    write(f.root / "source" / "nested" / "data.bin", "abc");
    write(f.root / "source" / "empty.bin", "");
    auto source = io::SourcePayload::open(f.root / "source", {1});
    REQUIRE(source);
    REQUIRE(source->manifest().kind == PayloadKind::folder);
    auto same = io::SourcePayload::open(f.root / "source", {1});
    REQUIRE(same);
    REQUIRE(same->manifest() == source->manifest());
    // A folder transfer is an explicit snapshot; later edits cannot mix bytes.
    write(f.root / "source" / "nested" / "data.bin", "changed");
    auto folder = io::FolderDestination::open(f.root / "dest", source->manifest(), false);
    REQUIRE(folder);
    REQUIRE_FALSE(io::FolderDestination::open(f.root / "dest", source->manifest(), true));
    auto partial = io::PartialFile::create(folder->payload_root(), source->manifest());
    REQUIRE(partial);
    receive(*source, *partial);
    std::stop_source cancelled;
    cancelled.request_stop();
    REQUIRE(folder->publish(cancelled.get_token()).error().code == ErrorCode::cancelled);
    REQUIRE_FALSE(std::filesystem::exists(f.root / "dest" / "source"));
    REQUIRE(folder->publish());
    REQUIRE(std::filesystem::is_directory(f.root / "dest" / "source" / "nested" / "empty"));
    REQUIRE(std::filesystem::file_size(f.root / "dest" / "source" / "empty.bin") == 0);
    auto final = io::SourceFile::open(f.root / "dest" / "source" / "nested" / "data.bin", {2});
    REQUIRE(final);
    REQUIRE(final->manifest().size == 3);
    REQUIRE(folder->publish().error().code == ErrorCode::destination_conflict);
}
TEST_CASE("folder checkpoint survives owner restart and resumes bundle bytes") {
    if (!io::supports_file_io())
        return;
    Fixture f;
    write(f.root / "source" / "large.bin", std::string(2 * chunk_size + 7, 'q'));
    auto source = io::SourcePayload::open(f.root / "source", {3});
    REQUIRE(source);
    {
        auto folder = io::FolderDestination::open(f.root / "dest", source->manifest(), false);
        REQUIRE(folder);
        auto partial = io::PartialFile::create(folder->payload_root(), source->manifest());
        REQUIRE(partial);
        auto chunk = source->read(0);
        REQUIRE(chunk);
        REQUIRE(*partial->append(chunk->data, chunk->digest) == chunk_size);
    }
    auto folder = io::FolderDestination::open(f.root / "dest", source->manifest(), true);
    REQUIRE(folder);
    auto partial = io::PartialFile::resume(folder->payload_root(), source->manifest());
    REQUIRE(partial);
    REQUIRE(partial->durable_bytes() == chunk_size);
    receive(*source, *partial);
    REQUIRE(folder->publish());
    REQUIRE(std::filesystem::file_size(f.root / "dest" / "source" / "large.bin") ==
            2 * chunk_size + 7);
}
TEST_CASE("folder source rejects symlinks special entries and case collisions") {
    if (!io::supports_file_io())
        return;
#ifndef _WIN32
    for (int mode = 0; mode < 6; ++mode) {
        Fixture f;
        if (mode == 0)
            std::filesystem::create_symlink(f.root / "dest", f.root / "source" / "link");
        if (mode == 1)
            REQUIRE(::mkfifo((f.root / "source" / "pipe").c_str(), 0600) == 0);
        if (mode == 2) {
            write(f.root / "source" / "A", "");
            write(f.root / "source" / "a", "");
            // A case-insensitive source stores one file for these two spellings.
            // Hostile received bundles still exercise collisions on every host.
            if (std::filesystem::equivalent(f.root / "source" / "A", f.root / "source" / "a")) {
                REQUIRE(io::SourcePayload::open(f.root / "source", {1}));
                continue;
            }
        }
        if (mode == 3)
            write(f.root / "source" / "CON.txt", "");
        if (mode == 4)
            write(f.root / "source" / "bad:stream", "");
        if (mode == 5)
            write(f.root / "source" / ".BRIDGE-private", "");
        REQUIRE_FALSE(io::SourcePayload::open(f.root / "source", {1}));
    }
    Fixture f;
    bridge::test::directory_link(f.root / "source", f.root / "alias");
    REQUIRE_FALSE(io::SourcePayload::open(f.root / "alias" / "", {1}));
    auto deep = f.root / "source";
    for (std::size_t i = 0; i <= io::maximum_folder_depth; ++i) {
        deep /= "d";
        std::filesystem::create_directory(deep);
    }
    REQUIRE_FALSE(io::SourcePayload::open(f.root / "source", {1}));
#else
    Fixture f;
    bridge::test::directory_link(f.root / "dest", f.root / "source" / "junction");
    REQUIRE_FALSE(io::SourcePayload::open(f.root / "source", {1}));
    bridge::test::directory_link(f.root / "source", f.root / "alias");
    REQUIRE_FALSE(io::SourcePayload::open(f.root / "alias" / "", {1}));

#endif
}
TEST_CASE("hostile folder bundles never publish final output") {
    if (!io::supports_file_io())
        return;
    for (int mode = 0; mode < 12; ++mode) {
        Fixture f;
        auto bytes = magic();
        switch (mode) {
        case 0:
            record(bytes, 2, "../escape");
            break;
        case 1:
            record(bytes, 2, "/absolute");
            break;
        case 2:
            record(bytes, 2, "C:stream");
            break;
        case 3:
            record(bytes, 2, "NUL.txt");
            break;
        case 4:
            record(bytes, 2, "missing/child");
            break;
        case 5:
            record(bytes, 1, "A");
            record(bytes, 1, "a");
            break;
        case 6:
            record(bytes, 3, "unknown");
            break;
        case 7:
            record(bytes, 2, "huge", maximum_file_size + 1);
            break;
        case 8:
            record(bytes, 2, "truncated", 4);
            break;
        case 9:
            record(bytes, 1, "directory", 1);
            break;
        case 10:
            record(bytes, 2, std::string(io::maximum_folder_path + 1, 'x'));
            break;
        case 11:
            break;
        }
        record(bytes, 0, {});
        if (mode == 11)
            bytes.push_back(99);
        FileManifest m{{1}, "result", bytes.size(), *security::sha256(bytes), PayloadKind::folder};
        auto folder = io::FolderDestination::open(f.root / "dest", m, false);
        REQUIRE(folder);
        write(folder->payload_root() / m.name,
              std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        REQUIRE_FALSE(folder->publish());
        REQUIRE_FALSE(std::filesystem::exists(f.root / "dest" / "result"));
        REQUIRE_FALSE(std::filesystem::exists(f.root / "dest" / "escape"));
    }
}
TEST_CASE("folder publication verifies hashes enforces bounds and pins destination handles") {
    if (!io::supports_file_io())
        return;
    Fixture f;
    auto bytes = magic();
    record(bytes, 2, "file", 1);
    bytes.push_back('a');
    record(bytes, 0, {});
    FileManifest m{{1}, "result", bytes.size(), *security::sha256(bytes), PayloadKind::folder};
    auto folder = io::FolderDestination::open(f.root / "dest", m, false);
    REQUIRE(folder);
    bytes[23] ^= 1;
    write(folder->payload_root() / m.name,
          std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    REQUIRE_FALSE(folder->publish());
    REQUIRE_FALSE(std::filesystem::exists(f.root / "dest" / "result"));
    bytes[23] ^= 1;
    write(folder->payload_root() / m.name,
          std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    auto published_root = f.root / "moved";
#ifdef _WIN32
    // Windows refuses to rename ancestors of open child files (our state lock).
    // This also prevents redirecting this live destination through a junction.
    std::error_code rename_error;
    std::filesystem::rename(f.root / "dest", published_root, rename_error);
    REQUIRE(rename_error == std::errc::permission_denied);
    published_root = f.root / "dest";
#else
    std::filesystem::rename(f.root / "dest", published_root);
#endif
    REQUIRE(folder->publish());
    REQUIRE(std::filesystem::exists(published_root / "result" / "file"));
    auto too_many = magic();
    for (std::size_t i = 0; i <= io::maximum_folder_entries; ++i)
        record(too_many, 1, "f" + std::to_string(i));
    record(too_many, 0, {});
    FileManifest huge{
        {2}, "many", too_many.size(), *security::sha256(too_many), PayloadKind::folder};
    auto destination = io::FolderDestination::open(published_root, huge, false);
    REQUIRE(destination);
    write(destination->payload_root() / huge.name,
          std::string_view(reinterpret_cast<const char*>(too_many.data()), too_many.size()));
    REQUIRE_FALSE(destination->publish());
    REQUIRE_FALSE(std::filesystem::exists(published_root / "many"));
}
TEST_CASE("complete folder bundle restarts extraction without trusting stale staging") {
    if (!io::supports_file_io())
        return;
    Fixture f;
    write(f.root / "source" / "nested" / "data", "verified");
    auto source = io::SourcePayload::open(f.root / "source", {4});
    REQUIRE(source);
    {
        auto folder = io::FolderDestination::open(f.root / "dest", source->manifest(), false);
        REQUIRE(folder);
        auto partial = io::PartialFile::create(folder->payload_root(), source->manifest());
        REQUIRE(partial);
        receive(*source, *partial);
        // Simulate an incomplete extraction and a hostile local reparse link at its
        // private staging name. Cleanup must unlink the link, not its target.
        std::filesystem::create_directory(f.root / "outside");
        write(f.root / "outside" / "keep", "keep");
        bridge::test::directory_link(f.root / "outside",
                                     folder->payload_root() / ".bridge-extract");
    }
    auto folder = io::FolderDestination::open(f.root / "dest", source->manifest(), true);
    REQUIRE(folder);
    auto partial = io::PartialFile::resume(folder->payload_root(), source->manifest());
    REQUIRE(partial);
    REQUIRE(partial->committed());
    REQUIRE(folder->publish());
    REQUIRE(std::filesystem::file_size(f.root / "dest" / "source" / "nested" / "data") == 8);
    REQUIRE(std::filesystem::exists(f.root / "outside" / "keep"));
}
