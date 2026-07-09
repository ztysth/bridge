#include "filesystem.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>
#include <utility>
namespace bridge::io::native {
namespace {
Error system_error(int code = errno) {
    switch (code) {
    case ENOSPC:
    case EDQUOT:
        return {ErrorCode::disk_full, code};
    case EACCES:
    case EPERM:
        return {ErrorCode::permission_denied, code};
    case EEXIST:
        return {ErrorCode::destination_conflict, code};
    case ELOOP:
    case ENOTDIR:
        return {ErrorCode::invalid_path, code};
    default:
        return {ErrorCode::io_failed, code};
    }
}
Metadata convert(const struct stat& value) {
    Metadata m;
    m.kind = S_ISREG(value.st_mode) && value.st_size >= 0 ? Kind::regular
             : S_ISDIR(value.st_mode)                     ? Kind::directory
                                                          : Kind::other;
    m.device = value.st_dev;
    m.identity = value.st_ino;
    m.size = value.st_size >= 0 ? static_cast<std::uint64_t>(value.st_size) : 0;
    m.links = value.st_nlink;
#ifdef __APPLE__
    const auto mt = value.st_mtimespec, ct = value.st_ctimespec;
#else
    const auto mt = value.st_mtim, ct = value.st_ctim;
#endif
    m.times = {mt.tv_sec, mt.tv_nsec, ct.tv_sec, ct.tv_nsec};
    return m;
}
Result<File> opened(int value, Kind kind) {
    File file(value);
    if (value < 0)
        return std::unexpected(system_error());
    auto m = metadata(value);
    if (!m)
        return std::unexpected(m.error());
    if (m->kind == Kind::other || (kind != Kind::other && m->kind != kind))
        return std::unexpected(Error{ErrorCode::invalid_path});
    return file;
}
struct DirCloser {
    void operator()(DIR* p) const { ::closedir(p); }
};
} // namespace
File::~File() {
    if (handle_ >= 0)
        ::close(handle_);
}
File::File(File&& other) noexcept
    : lock_file(std::move(other.lock_file)), handle_(std::exchange(other.handle_, invalid)) {}
File& File::operator=(File&& other) noexcept {
    File temporary(std::move(other));
    std::swap(handle_, temporary.handle_);
    lock_file.swap(temporary.lock_file);
    return *this;
}
Result<void> receive_root(Handle) { return {}; }
Result<File> open_path(const std::filesystem::path& path, Kind kind) {
    if (path.empty() || path.native().find('\0') != std::string::npos)
        return std::unexpected(Error{ErrorCode::invalid_path});
    return opened(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                                           (kind == Kind::directory ? O_DIRECTORY : 0)),
                  kind);
}
Result<File> open_at(Handle parent, const std::string& name, Access access, Kind kind) {
    if (access == Access::create_directory) {
        if (::mkdirat(parent, name.c_str(), 0700) != 0)
            return std::unexpected(system_error());
        kind = Kind::directory;
    }
    int flags = access == Access::update        ? O_RDWR
                : access == Access::create_file ? O_RDWR | O_CREAT | O_EXCL
                                                : O_RDONLY;
    return opened(::openat(parent, name.c_str(),
                           flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                               (kind == Kind::directory ? O_DIRECTORY : 0),
                           0600),
                  kind);
}
Result<File> duplicate(Handle value) {
    File copy(::dup(value));
    if (copy.get() < 0)
        return std::unexpected(system_error());
    return copy;
}
Result<Metadata> metadata(Handle file) {
    struct stat info {};
    if (::fstat(file, &info) != 0)
        return std::unexpected(system_error());
    return convert(info);
}
Result<std::optional<Metadata>> child_metadata(Handle parent, const std::string& name) {
    struct stat info {};
    if (::fstatat(parent, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT)
            return std::optional<Metadata>{};
        return std::unexpected(system_error());
    }
    return std::optional(convert(info));
}
Result<void> validate_private(Handle file) {
    struct stat info {};
    if (::fstat(file, &info) != 0)
        return std::unexpected(system_error());
    if (info.st_uid != ::geteuid() || (info.st_mode & 0777) != 0700)
        return std::unexpected(Error{ErrorCode::invalid_checkpoint});
    return {};
}
Result<void> lock(File& file) {
    if (::flock(file.get(), LOCK_EX | LOCK_NB) != 0)
        return std::unexpected(errno == EWOULDBLOCK ? Error{ErrorCode::checkpoint_busy}
                                                    : system_error());
    return {};
}
Result<void> read_at(Handle file, std::span<std::uint8_t> output, std::uint64_t offset,
                     ErrorCode end_error, std::stop_token stop) {
    static_assert(sizeof(off_t) >= 8);
    while (!output.empty()) {
        if (stop.stop_requested())
            return std::unexpected(Error{ErrorCode::cancelled});
        const auto count = ::pread(file, output.data(), output.size(), static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return std::unexpected(system_error());
        if (count == 0)
            return std::unexpected(Error{end_error});
        output = output.subspan(static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
    return {};
}
Result<void> write_at(Handle file, std::span<const std::uint8_t> input, std::uint64_t offset) {
    while (!input.empty()) {
        const auto count = ::pwrite(file, input.data(), input.size(), static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return std::unexpected(count < 0 ? system_error() : Error{ErrorCode::io_failed});
        input = input.subspan(static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
    return {};
}
Result<void> truncate(Handle file, std::uint64_t length) {
    if (::ftruncate(file, static_cast<off_t>(length)) != 0)
        return std::unexpected(system_error());
    return {};
}
Result<void> sync(Handle file) {
    int result;
    do {
        result = ::fsync(file);
    } while (result != 0 && errno == EINTR);
    if (result != 0)
        return std::unexpected(system_error());
    return {};
}
Result<void> remove(Handle parent, const std::string& name) {
    auto info = child_metadata(parent, name);
    if (!info)
        return std::unexpected(info.error());
    if (!*info)
        return {};
    if (::unlinkat(parent, name.c_str(), (**info).kind == Kind::directory ? AT_REMOVEDIR : 0) != 0)
        return std::unexpected(system_error());
    return {};
}
Result<void> link(Handle, Handle root, const std::string& from, const std::string& to) {
    if (::linkat(root, from.c_str(), root, to.c_str(), 0) != 0)
        return std::unexpected(system_error());
    return {};
}
Result<void> rename_directory(Handle, Handle state, Handle root, const std::string& name) {
#ifdef __linux__
    const auto result = ::syscall(SYS_renameat2, state, ".bridge-extract", root, name.c_str(), 1U);
#elif defined(__APPLE__)
    const auto result = ::renameatx_np(state, ".bridge-extract", root, name.c_str(), RENAME_EXCL);
#else
    return std::unexpected(Error{ErrorCode::unsupported_platform});
#endif
#if defined(__linux__) || defined(__APPLE__)
    if (result != 0)
        return std::unexpected(system_error());
    return sync(root);
#endif
}
Result<std::vector<Entry>> list(Handle file, std::size_t& count, std::size_t maximum,
                                std::stop_token stop) {
    const int copy = ::dup(file);
    if (copy < 0)
        return std::unexpected(system_error());
    std::unique_ptr<DIR, DirCloser> dir(::fdopendir(copy));
    if (!dir) {
        ::close(copy);
        return std::unexpected(system_error());
    }
    ::rewinddir(dir.get());
    std::vector<Entry> entries;
    while (true) {
        if (stop.stop_requested())
            return std::unexpected(Error{ErrorCode::cancelled});
        errno = 0;
        const auto* item = ::readdir(dir.get());
        if (!item) {
            if (errno != 0)
                return std::unexpected(system_error());
            break;
        }
        const std::string name(item->d_name);
        if (name == "." || name == "..")
            continue;
        if (++count > maximum)
            return std::unexpected(Error{ErrorCode::invalid_manifest});
        auto info = child_metadata(file, name);
        if (!info)
            return std::unexpected(info.error());
        if (!*info)
            return std::unexpected(Error{ErrorCode::source_changed});
        entries.push_back({name, **info});
    }
    std::ranges::sort(entries, {}, &Entry::name);
    return entries;
}
Result<std::filesystem::path> temporary_directory() {
    std::error_code ec;
    auto root = std::filesystem::temp_directory_path(ec);
    if (ec)
        return std::unexpected(Error{ErrorCode::io_failed, ec.value()});
    auto pattern = (root / "bridge-folder-XXXXXX").string();
    auto* created = ::mkdtemp(pattern.data());
    if (!created)
        return std::unexpected(system_error());
    return std::filesystem::path(created);
}
} // namespace bridge::io::native
