#include "bridge/io/path.hpp"
#include "bridge/io/source.hpp"
#include "bridge/security/hash.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
namespace bridge::io {
namespace {
Error storage_error() {
    return Error{errno == EACCES ? ErrorCode::permission_denied : ErrorCode::io_failed, errno};
}
bool unchanged(const struct stat& a, const struct stat& b) {
#ifdef __APPLE__
    const auto am = a.st_mtimespec, bm = b.st_mtimespec, ac = a.st_ctimespec, bc = b.st_ctimespec;
#else
    const auto am = a.st_mtim, bm = b.st_mtim, ac = a.st_ctim, bc = b.st_ctim;
#endif
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec && ac.tv_sec == bc.tv_sec &&
           ac.tv_nsec == bc.tv_nsec;
}
} // namespace
struct SourceFile::Impl {
    explicit Impl(int descriptor) : fd(descriptor) {}
    ~Impl() {
        if (fd >= 0)
            ::close(fd);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    int fd;
    struct stat snapshot {};
    FileManifest description;
    Result<void> stable() const {
        struct stat current {};
        if (::fstat(fd, &current) != 0)
            return std::unexpected(storage_error());
        if (!unchanged(snapshot, current))
            return std::unexpected(Error{ErrorCode::source_changed});
        return {};
    }
    Result<void> read_at(std::span<std::uint8_t> buffer, std::uint64_t offset,
                         std::stop_token stop) const {
        while (!buffer.empty()) {
            if (stop.stop_requested())
                return std::unexpected(Error{ErrorCode::cancelled});
            const auto count =
                ::pread(fd, buffer.data(), buffer.size(), static_cast<off_t>(offset));
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0)
                return std::unexpected(storage_error());
            if (count == 0)
                return std::unexpected(Error{ErrorCode::source_changed});
            auto length = static_cast<std::size_t>(count);
            offset += length;
            buffer = buffer.subspan(length);
        }
        return stable();
    }
};
SourceFile::SourceFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SourceFile::~SourceFile() = default;
SourceFile::SourceFile(SourceFile&&) noexcept = default;
SourceFile& SourceFile::operator=(SourceFile&&) noexcept = default;
bool supports_file_io() { return true; }
Result<SourceFile> SourceFile::open(const std::filesystem::path& path, TransferId id,
                                    std::stop_token stop) {
    static_assert(sizeof(off_t) >= 8);
    if (stop.stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    const auto name = path.filename().string();
    if (path.native().find('\0') != std::string::npos || name.find('/') != std::string::npos ||
        !validate_relative_path(name) || name.starts_with(".bridge-"))
        return std::unexpected(Error{ErrorCode::invalid_path});
    auto impl = std::make_unique<Impl>(-1);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return std::unexpected(storage_error());
    impl->fd = fd;
    if (::fstat(fd, &impl->snapshot) != 0)
        return std::unexpected(storage_error());
    if (!S_ISREG(impl->snapshot.st_mode) || impl->snapshot.st_size < 0)
        return std::unexpected(Error{ErrorCode::invalid_path});
    impl->description = {id, name, static_cast<std::uint64_t>(impl->snapshot.st_size), {}};
    auto valid = validate_manifest(impl->description);
    if (!valid)
        return std::unexpected(valid.error());
    auto hash = security::Sha256::create();
    if (!hash)
        return std::unexpected(hash.error());
    std::vector<std::uint8_t> buffer(chunk_size);
    for (std::uint64_t offset = 0; offset < impl->description.size;) {
        auto bytes = std::span(buffer).first(static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk_size, impl->description.size - offset)));
        auto read = impl->read_at(bytes, offset, stop);
        if (!read)
            return std::unexpected(read.error());
        auto updated = hash->update(bytes);
        if (!updated)
            return std::unexpected(updated.error());
        offset += bytes.size();
    }
    if (stop.stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    auto stable = impl->stable();
    if (!stable)
        return std::unexpected(stable.error());
    auto digest = hash->finish();
    if (!digest)
        return std::unexpected(digest.error());
    impl->description.digest = *digest;
    return SourceFile(std::move(impl));
}
const FileManifest& SourceFile::manifest() const { return impl_->description; }
Result<Chunk> SourceFile::read(std::uint64_t offset, std::stop_token stop) {
    if (offset % chunk_size != 0)
        return std::unexpected(Error{ErrorCode::invalid_state});
    auto stable = impl_->stable();
    if (!stable)
        return std::unexpected(stable.error());
    auto length = chunk_length(impl_->description.size, offset / chunk_size);
    if (!length)
        return std::unexpected(length.error());
    Chunk c{impl_->description.id, offset, {}, std::vector<std::uint8_t>(*length)};
    auto read = impl_->read_at(c.data, offset, stop);
    if (!read)
        return std::unexpected(read.error());
    auto digest = security::sha256(c.data);
    if (!digest)
        return std::unexpected(digest.error());
    c.digest = *digest;
    return c;
}
} // namespace bridge::io
