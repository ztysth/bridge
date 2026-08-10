#include "../../src/io/filesystem.hpp"
#include "../windows_junction.hpp"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
using namespace bridge;
namespace {
struct Fixture {
    std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("bridge-native-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { REQUIRE(std::filesystem::create_directory(root)); }
    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};
} // namespace
TEST_CASE("native handles provide bounded positional IO metadata and exclusive creation") {
    Fixture fixture;
    auto directory = io::native::open_root(fixture.root);
    REQUIRE(directory);
    auto private_dir =
        io::native::open_at(directory->get(), "private", io::native::Access::create_directory);
    REQUIRE(private_dir);
    REQUIRE(io::native::validate_private(private_dir->get()));
    auto file =
        io::native::open_at(private_dir->get(), "data.bin", io::native::Access::create_file);
    REQUIRE(file);
    REQUIRE_FALSE(
        io::native::open_at(private_dir->get(), "data.bin", io::native::Access::create_file));
    const std::array<std::uint8_t, 3> bytes{7, 9, 13};
    REQUIRE(io::native::write_at(file->get(), bytes, 0));
    REQUIRE(io::native::sync(file->get()));
    REQUIRE(io::native::metadata(file->get())->size == 3);
    std::array<std::uint8_t, 3> output{};
    REQUIRE(io::native::read_at(file->get(), output, 0));
    REQUIRE(output == bytes);
    REQUIRE_FALSE(io::native::read_at(file->get(), output, 3));
    std::stop_source stopped;
    stopped.request_stop();
    REQUIRE(
        io::native::read_at(file->get(), output, 0, ErrorCode::source_changed, stopped.get_token())
            .error()
            .code == ErrorCode::cancelled);
    REQUIRE(io::native::truncate(file->get(), 2));
    REQUIRE(io::native::metadata(file->get())->size == 2);
    std::size_t count = 0;
    REQUIRE(io::native::list(private_dir->get(), count, 1, {})->size() == 1);
    count = 0;
    REQUIRE_FALSE(io::native::list(private_dir->get(), count, 0, {}));
}
TEST_CASE("native directory opens reject junctions and remain pinned after root rename") {
    Fixture fixture, outside;
    auto root = io::native::open_root(fixture.root);
    REQUIRE(root);
    const auto moved = outside.root / "moved";
    std::filesystem::rename(fixture.root, moved);
    bridge::test::directory_link(outside.root, fixture.root);
    REQUIRE_FALSE(io::native::open_root(fixture.root));
    auto file = io::native::open_at(root->get(), "pinned.bin", io::native::Access::create_file);
    REQUIRE(file);
    REQUIRE(std::filesystem::exists(moved / "pinned.bin"));
    REQUIRE_FALSE(std::filesystem::exists(outside.root / "pinned.bin"));
    REQUIRE(std::filesystem::remove(fixture.root));
    fixture.root = moved;
}
TEST_CASE("native paths preserve Unicode ancestors without changing portable payload names") {
    Fixture fixture;
    const auto parent = fixture.root / std::filesystem::path(u8"folder-\u6d4b\u8bd5");
    REQUIRE(std::filesystem::create_directory(parent));
    auto root = io::native::open_root(parent);
    REQUIRE(root);
    auto file = io::native::open_at(root->get(), "payload", io::native::Access::create_file);
    REQUIRE(file);
    REQUIRE(std::filesystem::exists(parent / "payload"));
}
#ifdef _WIN32
TEST_CASE("Windows source and folder staging reject directory reparse points") {
    Fixture fixture, outside;
    bridge::test::directory_link(outside.root, fixture.root / "junction");
    auto root = io::native::open_root(fixture.root);
    REQUIRE(root);
    REQUIRE(io::native::child_metadata(root->get(), "junction")->value().kind ==
            io::native::Kind::other);
    REQUIRE_FALSE(io::native::open_at(root->get(), "junction", io::native::Access::read,
                                      io::native::Kind::directory));
    REQUIRE(io::native::remove(root->get(), "junction"));
    REQUIRE(std::filesystem::exists(outside.root));
}
#endif

#ifdef _WIN32
TEST_CASE("Windows malformed UTF-16 filenames return typed errors") {
    std::wstring invalid_name(1, static_cast<wchar_t>(0xd800));
    auto name = io::native::filename(std::filesystem::path(invalid_name));
    REQUIRE_FALSE(name);
    REQUIRE(name.error().code == ErrorCode::invalid_path);
}
#endif
TEST_CASE("native positional IO preserves offsets above four GiB with sparse storage") {
    Fixture fixture;
    auto root = io::native::open_root(fixture.root);
    REQUIRE(root);
    auto file = io::native::open_at(root->get(), "large.bin", io::native::Access::create_file);
    REQUIRE(file);
#ifdef _WIN32
    DWORD returned = 0;
    REQUIRE(
        DeviceIoControl(file->get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr));
#endif
    constexpr std::uint64_t offset = (5ULL << 30U) + 17;
    const std::array<std::uint8_t, 3> bytes{7, 19, 31};
    REQUIRE(io::native::write_at(file->get(), bytes, offset));
    REQUIRE(io::native::metadata(file->get())->size == offset + bytes.size());
    std::array<std::uint8_t, 3> output{};
    REQUIRE(io::native::read_at(file->get(), output, offset));
    REQUIRE(output == bytes);
    REQUIRE(io::native::truncate(file->get(), 0));
    REQUIRE(io::native::metadata(file->get())->size == 0);
}
