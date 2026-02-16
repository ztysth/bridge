#include "bridge/io/folder.hpp"
#include "bridge/io/path.hpp"
#include "bridge/security/hash.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <sys/file.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>
#include <utility>
namespace bridge::io {
namespace {
class Fd {
  public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    Fd(Fd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        Fd temporary(std::move(other));
        std::swap(fd_, temporary.fd_);
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    int release() { return std::exchange(fd_, -1); }

  private:
    int fd_;
};
struct DirCloser {
    void operator()(DIR* dir) const { ::closedir(dir); }
};
using Directory = std::unique_ptr<DIR, DirCloser>;
Error storage_error(int code = errno) {
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
std::string folded(std::string value) {
    for (auto& c : value)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return value;
}
Result<void> safe_path(const std::string& name) {
    if (!validate_relative_path(name) || name.size() > maximum_folder_path)
        return std::unexpected(Error{ErrorCode::invalid_path});
    std::size_t begin = 0, depth = 0;
    do {
        const auto end = name.find('/', begin);
        const auto component = name.substr(begin, end == std::string::npos ? end : end - begin);
        if (++depth > maximum_folder_depth || folded(component).starts_with(".bridge-"))
            return std::unexpected(Error{ErrorCode::invalid_path});
        if (end == std::string::npos)
            break;
        begin = end + 1;
    } while (true);
    return {};
}
std::filesystem::path normalized(std::filesystem::path path) {
    while (path.has_relative_path() && path.filename().empty())
        path = path.parent_path();
    return path;
}
Result<Fd> directory_at(int parent, const std::string& name) {
    Fd fd(::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (fd.get() < 0)
        return std::unexpected(storage_error());
    return fd;
}
Result<Fd> open_root(const std::filesystem::path& selected) {
    auto path = normalized(selected);
    if (path.empty() || path.native().find('\0') != std::string::npos)
        return std::unexpected(Error{ErrorCode::invalid_path});
    return directory_at(AT_FDCWD, path.string());
}
Result<void> sync_fd(int fd) {
    int result = 0;
    do {
        result = ::fsync(fd);
    } while (result != 0 && errno == EINTR);
    if (result != 0)
        return std::unexpected(storage_error());
    return {};
}
bool same_snapshot(const struct stat& a, const struct stat& b) {
#ifdef __APPLE__
    const auto am = a.st_mtimespec, bm = b.st_mtimespec, ac = a.st_ctimespec, bc = b.st_ctimespec;
#else
    const auto am = a.st_mtim, bm = b.st_mtim, ac = a.st_ctim, bc = b.st_ctim;
#endif
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec && ac.tv_sec == bc.tv_sec &&
           ac.tv_nsec == bc.tv_nsec;
}
Result<void> stable(int fd, const struct stat& before) {
    struct stat after {};
    if (::fstat(fd, &after) != 0)
        return std::unexpected(storage_error());
    if (!same_snapshot(before, after))
        return std::unexpected(Error{ErrorCode::source_changed});
    return {};
}
struct Entry {
    std::string name;
    struct stat info {};
};
Result<std::vector<Entry>> list(int fd, std::size_t& count, std::stop_token stop) {
    Fd copy(::dup(fd));
    if (copy.get() < 0)
        return std::unexpected(storage_error());
    // fdopendir assumes ownership only after success.
    Directory dir(::fdopendir(copy.get()));
    if (!dir)
        return std::unexpected(storage_error());
    static_cast<void>(copy.release());
    ::rewinddir(dir.get());
    std::vector<Entry> entries;
    while (true) {
        if (stop.stop_requested())
            return std::unexpected(Error{ErrorCode::cancelled});
        errno = 0;
        const auto* item = ::readdir(dir.get());
        if (!item) {
            if (errno != 0)
                return std::unexpected(storage_error());
            break;
        }
        const std::string name(item->d_name);
        if (name == "." || name == "..")
            continue;
        if (++count > maximum_folder_entries)
            return std::unexpected(Error{ErrorCode::invalid_manifest});
        Entry entry{name, {}};
        if (::fstatat(fd, name.c_str(), &entry.info, AT_SYMLINK_NOFOLLOW) != 0)
            return std::unexpected(storage_error());
        entries.push_back(std::move(entry));
    }
    std::ranges::sort(entries, {}, &Entry::name);
    return entries;
}
std::array<std::uint8_t, 11> header(std::uint8_t type, std::size_t length, std::uint64_t size) {
    std::array<std::uint8_t, 11> out{};
    out[0] = type;
    out[1] = static_cast<std::uint8_t>(length >> 8U);
    out[2] = static_cast<std::uint8_t>(length);
    for (unsigned i = 0; i < 8; ++i)
        out[3 + i] = static_cast<std::uint8_t>(size >> ((7U - i) * 8U));
    return out;
}
std::uint64_t number(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 0;
    for (auto byte : bytes)
        value = (value << 8U) | byte;
    return value;
}
struct Writer {
    int fd;
    std::uint64_t bytes = 0;
    Result<void> write(std::span<const std::uint8_t> input) {
        if (input.size() > maximum_file_size - bytes)
            return std::unexpected(Error{ErrorCode::invalid_manifest});
        bytes += input.size();
        while (!input.empty()) {
            const auto count = ::write(fd, input.data(), input.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return std::unexpected(count < 0 ? storage_error() : Error{ErrorCode::io_failed});
            input = input.subspan(static_cast<std::size_t>(count));
        }
        return {};
    }
};
Result<void> pack(int directory, const std::string& prefix, Writer& writer,
                  std::vector<std::uint8_t>& buffer, std::map<std::string, bool>& names,
                  std::size_t& count, std::stop_token stop) {
    struct stat before {};
    if (::fstat(directory, &before) != 0)
        return std::unexpected(storage_error());
    auto entries = list(directory, count, stop);
    if (!entries)
        return std::unexpected(entries.error());
    for (const auto& entry : *entries) {
        if (stop.stop_requested())
            return std::unexpected(Error{ErrorCode::cancelled});
        const auto path = prefix.empty() ? entry.name : prefix + '/' + entry.name;
        auto safe = safe_path(path);
        if (!safe)
            return safe;
        const bool is_directory = S_ISDIR(entry.info.st_mode);
        if ((!is_directory && !S_ISREG(entry.info.st_mode)) || entry.info.st_size < 0)
            return std::unexpected(Error{ErrorCode::invalid_path});
        if (!names.emplace(folded(path), is_directory).second)
            return std::unexpected(Error{ErrorCode::destination_conflict});
        Fd child(::openat(directory, entry.name.c_str(),
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                              (is_directory ? O_DIRECTORY : 0)));
        if (child.get() < 0)
            return std::unexpected(storage_error());
        auto unchanged = stable(child.get(), entry.info);
        if (!unchanged)
            return unchanged;
        const auto size = is_directory ? 0 : static_cast<std::uint64_t>(entry.info.st_size);
        if (size > maximum_file_size - writer.bytes)
            return std::unexpected(Error{ErrorCode::invalid_manifest});
        auto written = writer.write(header(is_directory ? 1 : 2, path.size(), size));
        if (!written)
            return written;
        written = writer.write(
            std::span(reinterpret_cast<const std::uint8_t*>(path.data()), path.size()));
        if (!written)
            return written;
        if (is_directory) {
            auto result = pack(child.get(), path, writer, buffer, names, count, stop);
            if (!result)
                return result;
        } else {
            for (std::uint64_t offset = 0; offset < size;) {
                if (stop.stop_requested())
                    return std::unexpected(Error{ErrorCode::cancelled});
                const auto amount =
                    static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
                const auto read =
                    ::pread(child.get(), buffer.data(), amount, static_cast<off_t>(offset));
                if (read < 0 && errno == EINTR)
                    continue;
                if (read <= 0)
                    return std::unexpected(read < 0 ? storage_error()
                                                    : Error{ErrorCode::source_changed});
                written = writer.write(std::span(buffer).first(static_cast<std::size_t>(read)));
                if (!written)
                    return written;
                offset += static_cast<std::uint64_t>(read);
            }
            unchanged = stable(child.get(), entry.info);
            if (!unchanged)
                return unchanged;
        }
    }
    return stable(directory, before);
}
struct Snapshot {
    std::filesystem::path directory;
    ~Snapshot() {
        if (!directory.empty()) {
            ::unlink((directory / "payload").c_str());
            ::rmdir(directory.c_str());
        }
    }
};
Result<void> collision(int root, const std::string& name) {
    std::size_t count = 0;
    // Destination enumeration is bounded too: avoid arbitrary work on a huge root.
    auto entries = list(root, count, {});
    if (!entries)
        return std::unexpected(entries.error());
    for (const auto& entry : *entries)
        if (folded(entry.name) == folded(name))
            return std::unexpected(Error{ErrorCode::destination_conflict});
    return {};
}
std::string state_name(const TransferId& id) {
    std::string name = ".bridge-";
    constexpr std::string_view hex = "0123456789abcdef";
    for (auto byte : id) {
        name.push_back(hex[byte >> 4U]);
        name.push_back(hex[byte & 15U]);
    }
    return name + ".folder-state";
}
Result<Fd> beneath(int root, const std::string& path) {
    Fd current(::dup(root));
    if (current.get() < 0)
        return std::unexpected(storage_error());
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        auto next = directory_at(current.get(),
                                 path.substr(begin, end == std::string::npos ? end : end - begin));
        if (!next)
            return std::unexpected(next.error());
        current = std::move(*next);
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return current;
}
// Only called beneath our exclusively locked, mode-0700 private namespace.
Result<void> remove_stage(int parent, const std::string& name, unsigned depth = 0) {
    if (depth > maximum_folder_depth)
        return std::unexpected(Error{ErrorCode::invalid_path});
    struct stat info {};
    if (::fstatat(parent, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT)
            return {};
        return std::unexpected(storage_error());
    }
    if (S_ISDIR(info.st_mode)) {
        auto dir = directory_at(parent, name);
        if (!dir)
            return std::unexpected(dir.error());
        std::size_t count = 0;
        auto entries = list(dir->get(), count, {});
        if (!entries)
            return std::unexpected(entries.error());
        for (const auto& entry : *entries) {
            auto removed = remove_stage(dir->get(), entry.name, depth + 1);
            if (!removed)
                return removed;
        }
    }
    if (::unlinkat(parent, name.c_str(), S_ISDIR(info.st_mode) ? AT_REMOVEDIR : 0) != 0)
        return std::unexpected(storage_error());
    return {};
}
struct Reader {
    int fd;
    std::uint64_t remaining;
    security::Sha256 digest;
    Result<void> read(std::span<std::uint8_t> output, std::stop_token stop) {
        if (output.size() > remaining)
            return std::unexpected(Error{ErrorCode::malformed_frame});
        while (!output.empty()) {
            if (stop.stop_requested())
                return std::unexpected(Error{ErrorCode::cancelled});
            const auto count = ::read(fd, output.data(), output.size());
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return std::unexpected(count < 0 ? storage_error()
                                                 : Error{ErrorCode::malformed_frame});
            auto bytes = output.first(static_cast<std::size_t>(count));
            auto updated = digest.update(bytes);
            if (!updated)
                return updated;
            remaining -= bytes.size();
            output = output.subspan(bytes.size());
        }
        return {};
    }
};
Result<void> extract(Reader& reader, int stage, std::stop_token stop) {
    std::array<std::uint8_t, 8> magic{};
    auto read = reader.read(magic, stop);
    constexpr std::array<std::uint8_t, 8> expected{'B', 'R', 'F', 'O', 'L', 'D', '0', '1'};
    if (!read)
        return read;
    if (magic != expected)
        return std::unexpected(Error{ErrorCode::malformed_frame});
    std::map<std::string, bool> names;
    std::vector<std::string> directories;
    std::vector<std::uint8_t> buffer(chunk_size);
    while (true) {
        std::array<std::uint8_t, 11> bytes{};
        read = reader.read(bytes, stop);
        if (!read)
            return read;
        const auto path_length = number(std::span(bytes).subspan(1, 2));
        const auto length = number(std::span(bytes).subspan(3, 8));
        if (bytes[0] == 0) {
            if (path_length != 0 || length != 0 || reader.remaining != 0)
                return std::unexpected(Error{ErrorCode::malformed_frame});
            break;
        }
        if ((bytes[0] != 1 && bytes[0] != 2) || path_length == 0 ||
            path_length > maximum_folder_path || length > maximum_file_size ||
            (bytes[0] == 1 && length != 0) || names.size() >= maximum_folder_entries)
            return std::unexpected(Error{ErrorCode::invalid_manifest});
        std::string path(static_cast<std::size_t>(path_length), '\0');
        read =
            reader.read(std::span(reinterpret_cast<std::uint8_t*>(path.data()), path.size()), stop);
        if (!read)
            return read;
        auto safe = safe_path(path);
        if (!safe)
            return safe;
        if (!names.emplace(folded(path), bytes[0] == 1).second)
            return std::unexpected(Error{ErrorCode::destination_conflict});
        const auto slash = path.rfind('/');
        const auto parent_path = slash == std::string::npos ? std::string{} : path.substr(0, slash);
        if (!parent_path.empty()) {
            const auto found = names.find(folded(parent_path));
            if (found == names.end() || !found->second)
                return std::unexpected(Error{ErrorCode::invalid_path});
        }
        auto parent = beneath(stage, parent_path);
        if (!parent)
            return std::unexpected(parent.error());
        const auto leaf = slash == std::string::npos ? path : path.substr(slash + 1);
        if (bytes[0] == 1) {
            if (::mkdirat(parent->get(), leaf.c_str(), 0700) != 0)
                return std::unexpected(storage_error());
            directories.push_back(path);
        } else {
            if (length > reader.remaining)
                return std::unexpected(Error{ErrorCode::malformed_frame});
            Fd file(::openat(parent->get(), leaf.c_str(),
                             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
            if (file.get() < 0)
                return std::unexpected(storage_error());
            Writer writer{file.get()};
            for (std::uint64_t left = length; left > 0;) {
                auto bytes_to_read = std::span(buffer).first(
                    static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), left)));
                read = reader.read(bytes_to_read, stop);
                if (!read)
                    return read;
                auto written = writer.write(bytes_to_read);
                if (!written)
                    return written;
                left -= bytes_to_read.size();
            }
            auto synced = sync_fd(file.get());
            if (!synced)
                return synced;
        }
    }
    for (auto i = directories.rbegin(); i != directories.rend(); ++i) {
        if (stop.stop_requested())
            return std::unexpected(Error{ErrorCode::cancelled});
        auto dir = beneath(stage, *i);
        if (!dir)
            return std::unexpected(dir.error());
        auto synced = sync_fd(dir->get());
        if (!synced)
            return synced;
    }
    return sync_fd(stage);
}
Result<void> publish_directory(int state, int root, const std::string& name) {
#ifdef __linux__
    const auto result = ::syscall(SYS_renameat2, state, ".bridge-extract", root, name.c_str(),
                                  1U /* RENAME_NOREPLACE */);
#elif defined(__APPLE__)
    const auto result = ::renameatx_np(state, ".bridge-extract", root, name.c_str(), RENAME_EXCL);
#else
    return std::unexpected(Error{ErrorCode::unsupported_platform});
#endif
#if defined(__linux__) || defined(__APPLE__)
    if (result != 0)
        return std::unexpected(storage_error());
    return sync_fd(root);
#endif
}
} // namespace
struct SourcePayload::Impl {
    std::unique_ptr<Snapshot> snapshot;
    SourceFile file;
    FileManifest description;
    Impl(std::unique_ptr<Snapshot> storage, SourceFile source, FileManifest manifest)
        : snapshot(std::move(storage)), file(std::move(source)), description(std::move(manifest)) {}
};
SourcePayload::SourcePayload(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SourcePayload::~SourcePayload() = default;
SourcePayload::SourcePayload(SourcePayload&&) noexcept = default;
SourcePayload& SourcePayload::operator=(SourcePayload&&) noexcept = default;
const FileManifest& SourcePayload::manifest() const { return impl_->description; }
Result<Chunk> SourcePayload::read(std::uint64_t offset, std::stop_token stop) {
    return impl_->file.read(offset, stop);
}
Result<SourcePayload> SourcePayload::open(const std::filesystem::path& selected, TransferId id,
                                          std::stop_token stop) {
    if (stop.stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    const auto path = normalized(selected);
    if (path.empty() || path.native().find('\0') != std::string::npos)
        return std::unexpected(Error{ErrorCode::invalid_path});
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0)
        return std::unexpected(storage_error());
    if (S_ISREG(info.st_mode)) {
        auto file = SourceFile::open(path, id, stop);
        if (!file)
            return std::unexpected(file.error());
        auto manifest = file->manifest();
        return SourcePayload(
            std::make_unique<Impl>(nullptr, std::move(*file), std::move(manifest)));
    }
    if (!S_ISDIR(info.st_mode) || !safe_path(path.filename().string()))
        return std::unexpected(Error{ErrorCode::invalid_path});
    auto root = open_root(path);
    if (!root)
        return std::unexpected(root.error());
    auto unchanged = stable(root->get(), info);
    if (!unchanged)
        return std::unexpected(unchanged.error());
    std::error_code ec;
    auto temporary = std::filesystem::temp_directory_path(ec);
    if (ec)
        return std::unexpected(Error{ErrorCode::io_failed, ec.value()});
    auto pattern = (temporary / "bridge-folder-XXXXXX").string();
    auto snapshot = std::make_unique<Snapshot>();
    auto* created = ::mkdtemp(pattern.data());
    if (!created)
        return std::unexpected(storage_error());
    snapshot->directory = created;
    Fd output(::open((snapshot->directory / "payload").c_str(),
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (output.get() < 0)
        return std::unexpected(storage_error());
    Writer writer{output.get()};
    constexpr std::array<std::uint8_t, 8> magic{'B', 'R', 'F', 'O', 'L', 'D', '0', '1'};
    auto written = writer.write(magic);
    if (!written)
        return std::unexpected(written.error());
    std::vector<std::uint8_t> buffer(chunk_size);
    std::map<std::string, bool> names;
    std::size_t count = 0;
    auto packed = pack(root->get(), {}, writer, buffer, names, count, stop);
    if (!packed)
        return std::unexpected(packed.error());
    written = writer.write(header(0, 0, 0));
    if (!written)
        return std::unexpected(written.error());
    unchanged = stable(root->get(), info);
    if (!unchanged)
        return std::unexpected(unchanged.error());
    auto file = SourceFile::open(snapshot->directory / "payload", id, stop);
    if (!file)
        return std::unexpected(file.error());
    auto manifest = file->manifest();
    manifest.name = path.filename().string();
    manifest.kind = PayloadKind::folder;
    return SourcePayload(
        std::make_unique<Impl>(std::move(snapshot), std::move(*file), std::move(manifest)));
}
struct FolderDestination::Impl {
    Fd root, state;
    std::filesystem::path payload;
    FileManifest manifest;
};
FolderDestination::FolderDestination(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FolderDestination::~FolderDestination() = default;
FolderDestination::FolderDestination(FolderDestination&&) noexcept = default;
FolderDestination& FolderDestination::operator=(FolderDestination&&) noexcept = default;
const std::filesystem::path& FolderDestination::payload_root() const { return impl_->payload; }
Result<FolderDestination> FolderDestination::open(const std::filesystem::path& selected,
                                                  const FileManifest& m, bool resume) {
    if (!validate_manifest(m) || m.kind != PayloadKind::folder || !safe_path(m.name) ||
        m.name.find('/') != std::string::npos)
        return std::unexpected(Error{ErrorCode::invalid_manifest});
    auto root = open_root(selected);
    if (!root)
        return std::unexpected(root.error());
    auto conflict = collision(root->get(), m.name);
    if (!conflict)
        return std::unexpected(conflict.error());
    const auto name = state_name(m.id);
    if (!resume && ::mkdirat(root->get(), name.c_str(), 0700) != 0)
        return std::unexpected(storage_error());
    auto state = directory_at(root->get(), name);
    if (!state)
        return std::unexpected(state.error());
    struct stat info {};
    if (::fstat(state->get(), &info) != 0)
        return std::unexpected(storage_error());
    if (info.st_uid != ::geteuid() || (info.st_mode & 0777) != 0700)
        return std::unexpected(Error{ErrorCode::invalid_checkpoint});
    if (::flock(state->get(), LOCK_EX | LOCK_NB) != 0)
        return std::unexpected(errno == EWOULDBLOCK ? Error{ErrorCode::checkpoint_busy}
                                                    : storage_error());
    auto synced = sync_fd(root->get());
    if (!synced)
        return std::unexpected(synced.error());
    auto impl = std::make_unique<Impl>();
    impl->root = std::move(*root);
    impl->state = std::move(*state);
    impl->payload = normalized(selected) / name;
    impl->manifest = m;
    return FolderDestination(std::move(impl));
}
Result<void> FolderDestination::publish(std::stop_token stop) {
    if (stop.stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    auto conflict = collision(impl_->root.get(), impl_->manifest.name);
    if (!conflict)
        return conflict;
    Fd bundle(::openat(impl_->state.get(), impl_->manifest.name.c_str(),
                       O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (bundle.get() < 0)
        return std::unexpected(storage_error());
    struct stat info {};
    if (::fstat(bundle.get(), &info) != 0)
        return std::unexpected(storage_error());
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) != impl_->manifest.size)
        return std::unexpected(Error{ErrorCode::invalid_checkpoint});
    auto removed = remove_stage(impl_->state.get(), ".bridge-extract");
    if (!removed)
        return removed;
    if (::mkdirat(impl_->state.get(), ".bridge-extract", 0700) != 0)
        return std::unexpected(storage_error());
    auto stage = directory_at(impl_->state.get(), ".bridge-extract");
    if (!stage)
        return std::unexpected(stage.error());
    auto digest = security::Sha256::create();
    if (!digest)
        return std::unexpected(digest.error());
    Reader reader{bundle.get(), impl_->manifest.size, std::move(*digest)};
    auto extracted = extract(reader, stage->get(), stop);
    if (!extracted)
        return extracted;
    auto hash = reader.digest.finish();
    if (!hash)
        return std::unexpected(hash.error());
    if (*hash != impl_->manifest.digest)
        return std::unexpected(Error{ErrorCode::checksum_mismatch});
    auto unchanged = stable(bundle.get(), info);
    if (!unchanged)
        return unchanged;
    if (stop.stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    conflict = collision(impl_->root.get(), impl_->manifest.name);
    if (!conflict)
        return conflict;
    auto published = publish_directory(impl_->state.get(), impl_->root.get(), impl_->manifest.name);
    if (!published)
        return published;
    return sync_fd(impl_->state.get());
}
} // namespace bridge::io
