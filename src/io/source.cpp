#include "bridge/io/source.hpp"
#include "bridge/io/path.hpp"
#include "bridge/security/hash.hpp"
#include "filesystem.hpp"
#include <algorithm>
namespace bridge::io {
struct SourceFile::Impl {
    explicit Impl(native::File descriptor) : fd(std::move(descriptor)) {}
    native::File fd;
    native::Metadata snapshot;
    FileManifest description;
    Result<void> stable() const { return native::stable(fd.get(), snapshot); }
    Result<void> read_at(std::span<std::uint8_t> buffer, std::uint64_t offset,
                         std::stop_token stop) const {
        auto read = native::read_at(fd.get(), buffer, offset, ErrorCode::source_changed, stop);
        if (!read)
            return read;
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
    if (stop.stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    const auto name = native::filename(path);
    if (path.native().find('\0') != std::string::npos || name.find('/') != std::string::npos ||
        !validate_relative_path(name) || name.starts_with(".bridge-"))
        return std::unexpected(Error{ErrorCode::invalid_path});
    auto file = native::open_path(path, native::Kind::regular);
    if (!file)
        return std::unexpected(file.error());
    auto impl = std::make_unique<Impl>(std::move(*file));
    auto info = native::metadata(impl->fd.get());
    if (!info)
        return std::unexpected(info.error());
    impl->snapshot = *info;
    impl->description = {id, name, info->size, {}};
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
