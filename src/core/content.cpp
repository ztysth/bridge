#include "bridge/core/content.hpp"
#include <algorithm>
namespace bridge {
Result<void> validate_manifest(const FileManifest& manifest) {
    if ((manifest.kind != PayloadKind::file && manifest.kind != PayloadKind::folder) ||
        manifest.name.empty() || manifest.name.size() > 255 || manifest.size > maximum_file_size ||
        std::ranges::all_of(manifest.id, [](auto byte) { return byte == 0; }))
        return std::unexpected(Error{ErrorCode::invalid_manifest});
    return {};
}
Result<std::uint32_t> chunk_length(std::uint64_t size, std::uint64_t index) {
    if (size > maximum_file_size || index >= chunk_count(size))
        return std::unexpected(Error{ErrorCode::invalid_manifest});
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(chunk_size, size - index * chunk_size));
}
Result<std::uint64_t> resume_index(std::uint64_t size, std::uint64_t durable_bytes) {
    if (size > maximum_file_size || durable_bytes > size ||
        (durable_bytes != size && durable_bytes % chunk_size != 0))
        return std::unexpected(Error{ErrorCode::invalid_checkpoint});
    return durable_bytes == size ? chunk_count(size) : durable_bytes / chunk_size;
}
} // namespace bridge
