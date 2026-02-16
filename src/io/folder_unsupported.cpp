#include "bridge/io/folder.hpp"
namespace bridge::io {
struct SourcePayload::Impl {};
SourcePayload::SourcePayload(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SourcePayload::~SourcePayload() = default;
SourcePayload::SourcePayload(SourcePayload&&) noexcept = default;
SourcePayload& SourcePayload::operator=(SourcePayload&&) noexcept = default;
Result<SourcePayload> SourcePayload::open(const std::filesystem::path&, TransferId,
                                          std::stop_token) {
    return std::unexpected(Error{ErrorCode::unsupported_platform});
}
const FileManifest& SourcePayload::manifest() const { std::terminate(); }
Result<Chunk> SourcePayload::read(std::uint64_t, std::stop_token) {
    return std::unexpected(Error{ErrorCode::unsupported_platform});
}
struct FolderDestination::Impl {};
FolderDestination::FolderDestination(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FolderDestination::~FolderDestination() = default;
FolderDestination::FolderDestination(FolderDestination&&) noexcept = default;
FolderDestination& FolderDestination::operator=(FolderDestination&&) noexcept = default;
Result<FolderDestination> FolderDestination::open(const std::filesystem::path&, const FileManifest&,
                                                  bool) {
    return std::unexpected(Error{ErrorCode::unsupported_platform});
}
const std::filesystem::path& FolderDestination::payload_root() const { std::terminate(); }
Result<void> FolderDestination::publish(std::stop_token) {
    return std::unexpected(Error{ErrorCode::unsupported_platform});
}
} // namespace bridge::io
