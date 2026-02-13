#include "bridge/io/checkpoint.hpp"
#include "bridge/security/hash.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace bridge;
namespace {
struct TemporaryRoot {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("bridge-io-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryRoot() { REQUIRE(std::filesystem::create_directory(path)); }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
Digest digest(std::span<const std::uint8_t> data) {
    auto value = security::sha256(data);
    REQUIRE(value);
    return *value;
}
FileManifest manifest(std::span<const std::uint8_t> data) {
    return {{1}, "received.bin", data.size(), digest(data)};
}
#ifndef _WIN32
std::filesystem::path checkpoint_path(const TemporaryRoot& root, std::string_view suffix) {
    for (const auto& entry : std::filesystem::directory_iterator(root.path))
        if (entry.path().filename().string().ends_with(suffix))
            return entry.path();
    FAIL("missing checkpoint fixture");
    return {};
}
void patch(const std::filesystem::path& path, std::uint64_t offset,
           std::span<const std::uint8_t> bytes) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}
void byte(const std::filesystem::path& path, std::uint64_t offset, std::uint8_t value) {
    patch(path, offset, std::span(&value, 1));
}
std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    const auto length = std::filesystem::file_size(path);
    REQUIRE(length <= chunk_size * 3ULL);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
    return bytes;
}
#endif
} // namespace
#ifdef _WIN32
TEST_CASE("Windows checkpoint IO fails closed until native handle support exists") {
    TemporaryRoot root;
    auto description = manifest({});
    REQUIRE(io::PartialFile::create(root.path, description).error().code ==
            ErrorCode::unsupported_platform);
    REQUIRE(io::PartialFile::resume(root.path, description).error().code ==
            ErrorCode::unsupported_platform);
    REQUIRE(std::filesystem::is_empty(root.path));
}
#else
TEST_CASE("checkpoint cancellation leaves data recoverable and never publishes early") {
    TemporaryRoot root;
    const std::array<std::uint8_t, 3> data{1, 2, 3};
    const auto m = manifest(data);
    std::stop_source stop;
    stop.request_stop();
    {
        auto writer = io::PartialFile::create(root.path, m);
        REQUIRE(writer);
        REQUIRE(writer->append(data, digest(data), stop.get_token()).error().code ==
                ErrorCode::cancelled);
        REQUIRE(writer->durable_bytes() == 0);
        REQUIRE(writer->append(data, digest(data)) == 3);
        REQUIRE(writer->finish(stop.get_token()).error().code == ErrorCode::cancelled);
        REQUIRE_FALSE(std::filesystem::exists(root.path / m.name));
    }
    REQUIRE(io::PartialFile::resume(root.path, m, {}, stop.get_token()).error().code ==
            ErrorCode::cancelled);
    auto restored = io::PartialFile::resume(root.path, m);
    REQUIRE(restored);
    REQUIRE(restored->durable_bytes() == 3);
    REQUIRE(restored->finish());
}
TEST_CASE("verified chunks resume and finalize without overwriting") {
    TemporaryRoot root;
    std::vector<std::uint8_t> data(chunk_size + 31, 37);
    const auto description = manifest(data);
    {
        auto writer = io::PartialFile::create(root.path, description);
        REQUIRE(writer);
        REQUIRE_FALSE(writer->finish());
        const auto first = std::span(data).first(chunk_size);
        REQUIRE(writer->append(first, digest(first)) == chunk_size);
        REQUIRE_FALSE(std::filesystem::exists(root.path / description.name));
        REQUIRE(io::PartialFile::resume(root.path, description).error().code ==
                ErrorCode::checkpoint_busy);
    }
    auto resumed = io::PartialFile::resume(root.path, description);
    REQUIRE(resumed);
    REQUIRE(resumed->durable_bytes() == chunk_size);
    const auto last = std::span(data).subspan(chunk_size);
    REQUIRE(resumed->append(last, digest(last)) == data.size());
    REQUIRE(resumed->finish());
    REQUIRE(resumed->committed());
    REQUIRE(read_file(root.path / description.name) == data);
    REQUIRE(std::distance(std::filesystem::directory_iterator(root.path),
                          std::filesystem::directory_iterator{}) == 1);
    REQUIRE_FALSE(resumed->append(last, digest(last)));
    REQUIRE_FALSE(resumed->finish());
    REQUIRE(io::PartialFile::create(root.path, description).error().code ==
            ErrorCode::destination_conflict);
    auto finished_resume = io::PartialFile::resume(root.path, description);
    REQUIRE(finished_resume);
    REQUIRE(finished_resume->committed());
    auto different = description;
    different.digest[0] ^= 1;
    REQUIRE(io::PartialFile::resume(root.path, different).error().code ==
            ErrorCode::destination_conflict);
}
TEST_CASE("empty files are verified and bad whole-file hashes never publish") {
    TemporaryRoot root;
    auto empty = manifest({});
    auto writer = io::PartialFile::create(root.path, empty);
    REQUIRE(writer);
    REQUIRE(writer->finish());
    REQUIRE(std::filesystem::file_size(root.path / empty.name) == 0);
    auto wrong = empty;
    wrong.name = "bad.bin";
    wrong.id[0] = 2;
    wrong.digest[0] ^= 1;
    auto bad = io::PartialFile::create(root.path, wrong);
    REQUIRE(bad);
    REQUIRE(bad->finish().error().code == ErrorCode::checksum_mismatch);
    REQUIRE_FALSE(std::filesystem::exists(root.path / wrong.name));
}
TEST_CASE("interrupted durability ordering retains only the verified prefix") {
    for (const auto fault :
         {io::CheckpointFault::after_data_sync, io::CheckpointFault::torn_record,
          io::CheckpointFault::after_record_sync, io::CheckpointFault::disk_full,
          io::CheckpointFault::permission_denied, io::CheckpointFault::partial_data_write}) {
        TemporaryRoot root;
        std::vector<std::uint8_t> data(chunk_size + 7, 19);
        auto description = manifest(data);
        {
            auto writer = io::PartialFile::create(root.path, description, {fault, 1});
            REQUIRE(writer);
            const auto first = std::span(data).first(chunk_size);
            auto failed = writer->append(first, digest(first));
            REQUIRE_FALSE(failed);
            const auto expected =
                fault == io::CheckpointFault::disk_full            ? ErrorCode::disk_full
                : fault == io::CheckpointFault::permission_denied  ? ErrorCode::permission_denied
                : fault == io::CheckpointFault::partial_data_write ? ErrorCode::io_failed
                                                                   : ErrorCode::injected_failure;
            REQUIRE(failed.error().code == expected);
            REQUIRE_FALSE(writer->append(first, digest(first)));
        }
        auto resumed = io::PartialFile::resume(root.path, description);
        REQUIRE(resumed);
        REQUIRE(resumed->durable_bytes() ==
                (fault == io::CheckpointFault::after_record_sync ? chunk_size : 0));
        for (auto offset = resumed->durable_bytes(); offset < data.size();) {
            auto part = std::span(data).subspan(static_cast<std::size_t>(offset),
                                                static_cast<std::size_t>(std::min<std::uint64_t>(
                                                    chunk_size, data.size() - offset)));
            auto appended = resumed->append(part, digest(part));
            REQUIRE(appended);
            offset = *appended;
        }
        REQUIRE(resumed->finish());
        REQUIRE(read_file(root.path / description.name) == data);
    }
}
TEST_CASE("publication interruption recovers with two names or final-only name") {
    for (bool remove_partial : {false, true}) {
        TemporaryRoot root;
        std::vector<std::uint8_t> data(73, 13);
        const auto description = manifest(data);
        {
            auto writer = io::PartialFile::create(root.path, description,
                                                  {io::CheckpointFault::after_publish, 1});
            REQUIRE(writer);
            REQUIRE(writer->append(data, digest(data)));
            REQUIRE(writer->finish().error().code == ErrorCode::injected_failure);
        }
        if (remove_partial)
            REQUIRE(std::filesystem::remove(checkpoint_path(root, ".part")));
        auto resumed = io::PartialFile::resume(root.path, description);
        REQUIRE(resumed);
        REQUIRE(resumed->committed());
        REQUIRE(read_file(root.path / description.name) == data);
        REQUIRE(std::distance(std::filesystem::directory_iterator(root.path),
                              std::filesystem::directory_iterator{}) == 1);
    }
}
TEST_CASE("hostile complete journal records and corrupted partials fail closed") {
    for (int mode = 0; mode < 10; ++mode) {
        TemporaryRoot root;
        std::vector<std::uint8_t> data(chunk_size * 2ULL, 61);
        const auto description = manifest(data);
        {
            auto writer = io::PartialFile::create(root.path, description);
            REQUIRE(writer);
            const auto first = std::span(data).first(chunk_size);
            REQUIRE(writer->append(first, digest(first)));
        }
        const auto journal = checkpoint_path(root, ".journal");
        const auto partial = checkpoint_path(root, ".part");
        const auto header_size = 100 + description.name.size();
        if (mode == 0)
            byte(journal, 6, 255);
        if (mode == 1)
            byte(journal, 5, 2);
        if (mode == 2)
            byte(journal, header_size - 1, 255);
        if (mode == 3)
            byte(journal, header_size + 79, 255);
        if (mode == 4 || mode == 5) {
            auto encoded = read_file(journal);
            auto record = std::span(encoded).subspan(header_size, 80);
            record[mode == 4 ? 11 : 15] = 7;
            const auto checksum = digest(record.first(48));
            std::ranges::copy(checksum, record.begin() + 48);
            patch(journal, header_size, record);
        }
        if (mode == 6)
            byte(partial, 0, 1);
        if (mode == 7)
            std::filesystem::resize_file(partial, chunk_size - 1);
        if (mode == 8)
            std::filesystem::resize_file(journal, 1ULL << 32U);
        if (mode == 9) {
            auto encoded = read_file(journal);
            patch(journal, encoded.size(), std::span(encoded).subspan(header_size, 80));
        }
        const auto old_journal_size = std::filesystem::file_size(journal);
        auto resumed = io::PartialFile::resume(root.path, description);
        REQUIRE_FALSE(resumed);
        REQUIRE(std::filesystem::file_size(journal) == old_journal_size);
        REQUIRE_FALSE(std::filesystem::exists(root.path / description.name));
    }
}
TEST_CASE("changed manifest and unverified or oversized chunks cannot change progress") {
    TemporaryRoot root;
    std::vector<std::uint8_t> data(93, 29);
    const auto description = manifest(data);
    {
        auto writer = io::PartialFile::create(root.path, description);
        REQUIRE(writer);
        REQUIRE(writer->append(std::span(data).first(1), digest(data)).error().code ==
                ErrorCode::invalid_manifest);
        REQUIRE(writer->append(data, Digest{}).error().code == ErrorCode::checksum_mismatch);
        REQUIRE(writer->durable_bytes() == 0);
    }
    for (int mode = 0; mode < 4; ++mode) {
        auto changed = description;
        if (mode == 0)
            changed.size++;
        if (mode == 1)
            changed.name = "alternate.bin";
        if (mode == 2)
            changed.digest[0] ^= 1;
        if (mode == 3)
            changed.size = maximum_file_size + 1;
        REQUIRE_FALSE(io::PartialFile::resume(root.path, changed));
    }
    auto resumed = io::PartialFile::resume(root.path, description);
    REQUIRE(resumed);
    REQUIRE(resumed->append(data, digest(data)));
}
TEST_CASE("native storage errors preserve redacted numeric context") {
    TemporaryRoot root;
    auto failure = io::PartialFile::create(root.path / "missing", manifest({}));
    REQUIRE_FALSE(failure);
    REQUIRE(failure.error().code == ErrorCode::io_failed);
    REQUIRE(failure.error().native_code == ENOENT);
}
TEST_CASE("destination conflicts including symlinks never overwrite") {
    for (bool before_create : {false, true}) {
        TemporaryRoot root, outside;
        const std::vector<std::uint8_t> data(5, 1);
        const auto description = manifest(data);
        const auto target = outside.path / "original";
        {
            std::ofstream file(target);
            file << "preserve";
        }
        if (before_create) {
            std::filesystem::create_symlink(target, root.path / description.name);
            REQUIRE(io::PartialFile::create(root.path, description).error().code ==
                    ErrorCode::destination_conflict);
        } else {
            auto writer = io::PartialFile::create(root.path, description);
            REQUIRE(writer);
            REQUIRE(writer->append(data, digest(data)));
            std::filesystem::create_symlink(target, root.path / description.name);
            REQUIRE(writer->finish().error().code == ErrorCode::destination_conflict);
        }
        const auto original = read_file(target);
        REQUIRE(std::string(original.begin(), original.end()) == "preserve");
    }
}
TEST_CASE("symlink hardlink and special checkpoint objects cannot escape root") {
    for (int mode = 0; mode < 4; ++mode) {
        TemporaryRoot root, outside;
        const std::vector<std::uint8_t> data(23, 41);
        const auto description = manifest(data);
        { REQUIRE(io::PartialFile::create(root.path, description)); }
        auto path = checkpoint_path(root, mode == 1 ? ".journal" : ".part");
        if (mode == 2) {
            std::filesystem::create_hard_link(path, outside.path / "linked");
        } else {
            REQUIRE(std::filesystem::remove(path));
            if (mode == 3)
                REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
            else
                std::filesystem::create_symlink(outside.path / "untouched", path);
        }
        REQUIRE_FALSE(io::PartialFile::resume(root.path, description));
        REQUIRE_FALSE(std::filesystem::exists(outside.path / "untouched"));
    }
}
TEST_CASE("flat portable names and root handles resist path redirection") {
    TemporaryRoot root, outside;
    const auto description = manifest({});
    for (const auto name : {"../escape", "a/b", "/abs", "C:stream", "NUL.txt", "a\\b",
                            ".BRIDGE-foo", "trailing.", ""}) {
        auto invalid = description;
        invalid.name = name;
        REQUIRE_FALSE(io::PartialFile::create(root.path, invalid));
    }
    const auto alias = outside.path / "root-link";
    std::filesystem::create_directory_symlink(root.path, alias);
    REQUIRE_FALSE(io::PartialFile::create(alias, description));
    REQUIRE_FALSE(
        io::PartialFile::create(std::filesystem::path(alias.string() + "/"), description));
    REQUIRE_FALSE(
        io::PartialFile::create(std::filesystem::path(alias.string() + "///"), description));
    auto writer =
        io::PartialFile::create(std::filesystem::path(root.path.string() + "/"), description);
    REQUIRE(writer);
    const auto original = root.path;
    root.path = outside.path / "moved";
    std::filesystem::rename(original, root.path);
    std::filesystem::create_directory_symlink(outside.path, original);
    REQUIRE(writer->finish());
    REQUIRE(std::filesystem::exists(root.path / description.name));
    REQUIRE_FALSE(std::filesystem::exists(outside.path / description.name));
    REQUIRE(std::filesystem::remove(original));
}
TEST_CASE("large logical files preserve 64-bit manifests without allocating file-sized buffers") {
    TemporaryRoot root;
    FileManifest description{{1}, "large.bin", (5ULL << 30U) + 17, {}};
    const std::vector<std::uint8_t> first(chunk_size, 53);
    {
        auto writer = io::PartialFile::create(root.path, description);
        REQUIRE(writer);
        REQUIRE(writer->append(first, digest(first)) == chunk_size);
        REQUIRE(std::filesystem::file_size(checkpoint_path(root, ".part")) == chunk_size);
    }
    auto resumed = io::PartialFile::resume(root.path, description);
    REQUIRE(resumed);
    REQUIRE(resumed->durable_bytes() == chunk_size);
    REQUIRE_FALSE(resumed->finish());
}
#endif
