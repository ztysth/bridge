#pragma once
#include "bridge/io/source.hpp"
namespace bridge::io {
inline constexpr std::size_t maximum_folder_entries = 4096;
inline constexpr std::size_t maximum_folder_path = 1024;
inline constexpr std::size_t maximum_folder_depth = 32;
// Pins a regular source, or owns a private disk snapshot of a directory tree.
// All blocking preparation/read/destruction belongs on the IO worker.
class SourcePayload {
  public:
    static Result<SourcePayload> open(const std::filesystem::path& path, TransferId id,
                                      std::stop_token stop = {});
    ~SourcePayload();
    SourcePayload(SourcePayload&&) noexcept;
    SourcePayload& operator=(SourcePayload&&) noexcept;
    const FileManifest& manifest() const;
    Result<Chunk> read(std::uint64_t offset, std::stop_token stop = {});

  private:
    struct Impl;
    explicit SourcePayload(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
// Owns a pinned destination and an exclusive private bundle checkpoint namespace.
// Publication strictly parses a verified bundle into private no-follow staging.
class FolderDestination {
  public:
    static Result<FolderDestination> open(const std::filesystem::path& root,
                                          const FileManifest& manifest, bool resume);
    ~FolderDestination();
    FolderDestination(FolderDestination&&) noexcept;
    FolderDestination& operator=(FolderDestination&&) noexcept;
    const std::filesystem::path& payload_root() const;
    Result<void> publish(std::stop_token stop = {});

  private:
    struct Impl;
    explicit FolderDestination(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace bridge::io
