#pragma once
#include "bridge/core/content.hpp"
#include <filesystem>
#include <memory>
#include <stop_token>
namespace bridge::io {
enum class CheckpointFault {
    none,
    after_data_sync,
    torn_record,
    after_record_sync,
    after_publish,
    disk_full,
    permission_denied,
    partial_data_write
};
// Local diagnostic injection only; never set from peer input or production UI.
struct CheckpointInjection {
    CheckpointFault point = CheckpointFault::none;
    unsigned hit = 1;
};
class PartialFile {
  public:
    static Result<PartialFile> create(const std::filesystem::path& root,
                                      const FileManifest& manifest,
                                      CheckpointInjection injection = {});
    static Result<PartialFile> resume(const std::filesystem::path& root,
                                      const FileManifest& manifest,
                                      CheckpointInjection injection = {},
                                      std::stop_token stop = {});
    ~PartialFile();
    PartialFile(PartialFile&&) noexcept;
    PartialFile& operator=(PartialFile&&) noexcept;
    // Blocking IO: schedule off the GUI thread. One owner must serialize all
    // calls; never invoke append/finish concurrently on this object.
    Result<std::uint64_t> append(std::span<const std::uint8_t> data, const Digest& chunk_digest,
                                 std::stop_token stop = {});
    Result<void> finish(std::stop_token stop = {});
    std::uint64_t durable_bytes() const;
    bool committed() const;

  private:
    struct Impl;
    explicit PartialFile(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace bridge::io
