#pragma once
#include "bridge/core/file_protocol.hpp"
#include <filesystem>
#include <memory>
#include <stop_token>
namespace bridge::io {
// Local source pathname is user selected. Final symlinks/special files are rejected.
// The pinned descriptor and manifest live exclusively on the IO worker.
class SourceFile {
  public:
    static Result<SourceFile> open(const std::filesystem::path& path, TransferId id,
                                   std::stop_token stop = {});
    ~SourceFile();
    SourceFile(SourceFile&&) noexcept;
    SourceFile& operator=(SourceFile&&) noexcept;
    const FileManifest& manifest() const;
    Result<Chunk> read(std::uint64_t offset, std::stop_token stop = {});

  private:
    struct Impl;
    explicit SourceFile(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
bool supports_file_io();
} // namespace bridge::io
