#pragma once
#include "bridge/core/content.hpp"
#include "bridge/core/frame.hpp"
#include "bridge/core/session.hpp"
namespace bridge {
struct Prefix {
    TransferId id;
    std::uint64_t bytes;
};
struct Chunk {
    TransferId id;
    std::uint64_t offset;
    Digest digest;
    std::vector<std::uint8_t> data;
};
struct Hold {
    std::uint64_t revision;
    bool paused;
};
struct Barrier {
    std::uint64_t sender_revision;
    std::uint64_t receiver_revision;
    std::uint64_t bytes;
    bool operator==(const Barrier&) const = default;
};
std::vector<std::uint8_t> encode_offer(const FileManifest& manifest);
Result<FileManifest> decode_offer(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_prefix(const Prefix& prefix);
Result<Prefix> decode_prefix(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_chunk(const Chunk& chunk);
Result<Chunk> decode_chunk(std::span<const std::uint8_t> bytes, const FileManifest& manifest);
std::vector<std::uint8_t> encode_hold(Hold hold);
Result<Hold> decode_hold(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode_barrier(Barrier barrier);
Result<Barrier> decode_barrier(std::span<const std::uint8_t> bytes);
class PauseState {
  public:
    explicit PauseState(Role role) : role_(role) {}
    Result<Hold> pause();
    Result<Hold> continue_transfer();
    Result<void> receive(Hold hold);
    Barrier barrier(std::uint64_t bytes) const;
    // Older crossing barriers are ignored, future revisions are fatal.
    Result<bool> current(Barrier barrier) const;
    void acknowledge();
    bool held() const { return local_ || remote_; }
    bool paused() const { return acknowledged_ && held(); }
    bool local_held() const { return local_; }
    bool remote_held() const { return remote_; }

  private:
    Role role_;
    std::uint64_t local_revision_ = 0, remote_revision_ = 0;
    bool local_ = false, remote_ = false, acknowledged_ = false, remote_confirmed_ = false;
};
} // namespace bridge
