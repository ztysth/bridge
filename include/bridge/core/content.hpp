#pragma once
#include "bridge/core/error.hpp"
#include <array>
#include <span>
#include <string>
namespace bridge {
using Digest = std::array<std::uint8_t, 32>;
using TransferId = std::array<std::uint8_t, 16>;
inline constexpr std::uint32_t chunk_size = 1048576;
inline constexpr std::uint64_t maximum_file_size = 1ULL << 40U;
enum class PayloadKind : std::uint8_t { file = 0, folder = 1 };
struct FileManifest {
    TransferId id;
    std::string name;
    std::uint64_t size;
    Digest digest;
    PayloadKind kind = PayloadKind::file;
    bool operator==(const FileManifest&) const = default;
};
Result<void> validate_manifest(const FileManifest& manifest);
constexpr std::uint64_t chunk_count(std::uint64_t size) {
    return size / chunk_size + (size % chunk_size != 0 ? 1U : 0U);
}
Result<std::uint32_t> chunk_length(std::uint64_t size, std::uint64_t index);
Result<std::uint64_t> resume_index(std::uint64_t size, std::uint64_t durable_bytes);
} // namespace bridge
