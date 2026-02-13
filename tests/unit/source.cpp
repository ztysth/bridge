#include "bridge/io/source.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
using namespace bridge;
TEST_CASE("source pins regular files detects mutation and supports cancellation") {
    if (!io::supports_file_io()) {
        REQUIRE_FALSE(io::SourceFile::open("unsupported", {1}));
        return;
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("bridge-source-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(root);
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::filesystem::remove_all(root); }
    } cleanup{root};
    auto path = root / "data.bin";
    {
        std::ofstream file(path);
        file << "abc";
    }
    auto source = io::SourceFile::open(path, {1});
    REQUIRE(source);
    REQUIRE(source->manifest().size == 3);
    REQUIRE(source->read(0)->data.size() == 3);
    REQUIRE_FALSE(source->read(1));
    std::stop_source cancelled;
    cancelled.request_stop();
    REQUIRE(io::SourceFile::open(path, {1}, cancelled.get_token()).error().code ==
            ErrorCode::cancelled);
    REQUIRE(source->read(0, cancelled.get_token()).error().code == ErrorCode::cancelled);
    {
        std::ofstream file(path, std::ios::app);
        file << "d";
    }
    REQUIRE(source->read(0).error().code == ErrorCode::source_changed);
    REQUIRE_FALSE(io::SourceFile::open(root, {1}));
#ifndef _WIN32
    std::filesystem::create_symlink(path, root / "alias");
    REQUIRE_FALSE(io::SourceFile::open(root / "alias", {1}));
#endif
}
