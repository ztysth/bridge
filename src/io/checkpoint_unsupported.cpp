#include "bridge/io/checkpoint.hpp"
namespace bridge::io {
struct PartialFile::Impl {};
PartialFile::PartialFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PartialFile::~PartialFile() = default;
PartialFile::PartialFile(PartialFile&&) noexcept = default;
PartialFile& PartialFile::operator=(PartialFile&&) noexcept = default;
Result<PartialFile> PartialFile::create(const std::filesystem::path&, const FileManifest&,
                                        CheckpointInjection) {
    return std::unexpected(Error{ErrorCode::unsupported_platform});
}
Result<PartialFile> PartialFile::resume(const std::filesystem::path&, const FileManifest&,
                                        CheckpointInjection, std::stop_token) {
    return std::unexpected(Error{ErrorCode::unsupported_platform});
}
Result<std::uint64_t> PartialFile::append(std::span<const std::uint8_t>, const Digest&,
                                          std::stop_token) {
    return std::unexpected(Error{ErrorCode::unsupported_platform});
}
Result<void> PartialFile::finish(std::stop_token) {
    return std::unexpected(Error{ErrorCode::unsupported_platform});
}
std::uint64_t PartialFile::durable_bytes() const { return 0; }
bool PartialFile::committed() const { return false; }
} // namespace bridge::io
