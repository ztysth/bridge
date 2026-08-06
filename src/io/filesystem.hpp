#pragma once
#include "bridge/core/error.hpp"
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace bridge::io::native {
#ifdef _WIN32
using Handle = void*;
inline constexpr Handle invalid = nullptr;
#else
using Handle = int;
inline constexpr Handle invalid = -1;
#endif
// Handles are exclusively owned; get() is borrowed and never retained by callers.
class File {
  public:
    explicit File(Handle handle = invalid) : handle_(handle) {}
    ~File();
    File(File&&) noexcept;
    File& operator=(File&&) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    Handle get() const { return handle_; }
    // Windows directory locks use an owned lock file in the private namespace.
    std::unique_ptr<File> lock_file;

  private:
    Handle handle_;
};
enum class Kind { regular, directory, other };
enum class Access { read, update, create_file, create_directory };
struct Metadata {
    Kind kind = Kind::other;
    std::uint64_t device = 0, identity = 0, size = 0, links = 0;
    std::array<std::int64_t, 4> times{};
};
struct Entry {
    std::string name;
    Metadata info;
};
std::filesystem::path normalized(std::filesystem::path);
Result<std::string> filename(const std::filesystem::path&);
bool same_file(const Metadata&, const Metadata&);
bool same_snapshot(const Metadata&, const Metadata&);
Result<File> open_path(const std::filesystem::path&, Kind);
Result<void> receive_root(Handle);
Result<File> open_root(const std::filesystem::path&);
Result<File> open_at(Handle, const std::string&, Access, Kind = Kind::regular);
Result<File> duplicate(Handle);
Result<Metadata> metadata(Handle);
Result<std::optional<Metadata>> child_metadata(Handle, const std::string&);
Result<void> stable(Handle, const Metadata&);
Result<void> validate_private(Handle);
Result<void> lock(File&);
Result<void> read_at(Handle, std::span<std::uint8_t>, std::uint64_t,
                     ErrorCode end_error = ErrorCode::invalid_checkpoint, std::stop_token = {});
Result<void> write_at(Handle, std::span<const std::uint8_t>, std::uint64_t);
Result<void> truncate(Handle, std::uint64_t);
Result<void> sync(Handle);
Result<void> remove(Handle, const std::string&);
Result<void> link(Handle file, Handle root, const std::string& from, const std::string& to);
Result<void> rename_directory(Handle stage, Handle state, Handle root, const std::string&);
Result<std::vector<Entry>> list(Handle, std::size_t&, std::size_t maximum, std::stop_token);
Result<std::filesystem::path> temporary_directory();
} // namespace bridge::io::native
