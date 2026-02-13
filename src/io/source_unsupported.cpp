#include "bridge/io/source.hpp"
namespace bridge::io {
struct SourceFile::Impl {
    FileManifest manifest;
};
SourceFile::SourceFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SourceFile::~SourceFile() = default;
SourceFile::SourceFile(SourceFile&&) noexcept = default;
SourceFile& SourceFile::operator=(SourceFile&&) noexcept = default;
bool supports_file_io() { return false; }
Result<SourceFile> SourceFile::open(const std::filesystem::path&, TransferId, std::stop_token) {
    return std::unexpected(Error{ErrorCode::unsupported_platform});
}
const FileManifest& SourceFile::manifest() const { return impl_->manifest; }
Result<Chunk> SourceFile::read(std::uint64_t, std::stop_token) {
    return std::unexpected(Error{ErrorCode::unsupported_platform});
}
} // namespace bridge::io
