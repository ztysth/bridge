#include "bridge/core/file_protocol.hpp"
#include <algorithm>
#include <limits>
namespace bridge {
namespace {
void put(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned width) {
    for (unsigned i = width; i > 0; --i)
        out.push_back(static_cast<std::uint8_t>(value >> ((i - 1) * 8U)));
}
std::uint64_t get(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 0;
    for (auto byte : bytes)
        value = (value << 8U) | byte;
    return value;
}
Error malformed() { return Error{ErrorCode::malformed_frame}; }
} // namespace
std::vector<std::uint8_t> encode_offer(const FileManifest& m) {
    std::vector<std::uint8_t> out(m.id.begin(), m.id.end());
    put(out, m.size, 8);
    put(out, chunk_size, 4);
    out.insert(out.end(), m.digest.begin(), m.digest.end());
    put(out, m.name.size(), 2);
    out.push_back(static_cast<std::uint8_t>(m.kind));
    out.insert(out.end(), m.name.begin(), m.name.end());
    return out;
}
Result<FileManifest> decode_offer(std::span<const std::uint8_t> b) {
    if (b.size() < 64 || b.size() > 318 || get(b.subspan(24, 4)) != chunk_size ||
        get(b.subspan(60, 2)) != b.size() - 63)
        return std::unexpected(malformed());
    FileManifest m;
    std::copy_n(b.begin(), 16, m.id.begin());
    m.size = get(b.subspan(16, 8));
    std::copy_n(b.begin() + 28, 32, m.digest.begin());
    m.kind = static_cast<PayloadKind>(b[62]);
    m.name.assign(b.begin() + 63, b.end());
    auto valid = validate_manifest(m);
    if (!valid)
        return std::unexpected(valid.error());
    return m;
}
std::vector<std::uint8_t> encode_prefix(const Prefix& p) {
    std::vector<std::uint8_t> out(p.id.begin(), p.id.end());
    put(out, p.bytes, 8);
    return out;
}
Result<Prefix> decode_prefix(std::span<const std::uint8_t> b) {
    if (b.size() != 24)
        return std::unexpected(malformed());
    Prefix p{};
    std::copy_n(b.begin(), 16, p.id.begin());
    p.bytes = get(b.subspan(16, 8));
    return p;
}
std::vector<std::uint8_t> encode_chunk(const Chunk& c) {
    std::vector<std::uint8_t> out(c.id.begin(), c.id.end());
    put(out, c.offset, 8);
    put(out, c.data.size(), 4);
    out.insert(out.end(), c.digest.begin(), c.digest.end());
    out.insert(out.end(), c.data.begin(), c.data.end());
    return out;
}
Result<Chunk> decode_chunk(std::span<const std::uint8_t> b, const FileManifest& m) {
    if (b.size() < 61 || b.size() > max_file_payload)
        return std::unexpected(malformed());
    auto p = decode_prefix(b.first(24));
    if (!p || p->id != m.id || p->bytes >= m.size || p->bytes % chunk_size != 0 ||
        get(b.subspan(24, 4)) != b.size() - 60)
        return std::unexpected(malformed());
    auto length = chunk_length(m.size, p->bytes / chunk_size);
    if (!length || *length != b.size() - 60)
        return std::unexpected(malformed());
    Chunk c{p->id, p->bytes, {}, {}};
    std::copy_n(b.begin() + 28, 32, c.digest.begin());
    c.data.assign(b.begin() + 60, b.end());
    return c;
}
std::vector<std::uint8_t> encode_hold(Hold h) {
    std::vector<std::uint8_t> out;
    put(out, h.revision, 8);
    out.push_back(h.paused ? 1 : 0);
    return out;
}
Result<Hold> decode_hold(std::span<const std::uint8_t> b) {
    if (b.size() != 9 || b[8] > 1 || get(b.first(8)) == 0)
        return std::unexpected(malformed());
    return Hold{get(b.first(8)), b[8] == 1};
}
std::vector<std::uint8_t> encode_barrier(Barrier b) {
    std::vector<std::uint8_t> out;
    put(out, b.sender_revision, 8);
    put(out, b.receiver_revision, 8);
    put(out, b.bytes, 8);
    return out;
}
Result<Barrier> decode_barrier(std::span<const std::uint8_t> b) {
    if (b.size() != 24)
        return std::unexpected(malformed());
    return Barrier{get(b.first(8)), get(b.subspan(8, 8)), get(b.subspan(16, 8))};
}
Result<Hold> PauseState::pause() {
    if (local_ || paused() || local_revision_ == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::invalid_state});
    local_ = true;
    acknowledged_ = false;
    return Hold{++local_revision_, true};
}
Result<Hold> PauseState::continue_transfer() {
    if (!paused() || !local_ || local_revision_ == std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::invalid_state});
    local_ = false;
    acknowledged_ = false;
    return Hold{++local_revision_, false};
}
Result<void> PauseState::receive(Hold h) {
    if (remote_revision_ == std::numeric_limits<std::uint64_t>::max() ||
        h.revision != remote_revision_ + 1 || h.paused == remote_ ||
        (!h.paused && !remote_confirmed_))
        return std::unexpected(Error{ErrorCode::invalid_state});
    remote_revision_ = h.revision;
    remote_ = h.paused;
    acknowledged_ = false;
    if (h.paused)
        remote_confirmed_ = false;
    return {};
}
Barrier PauseState::barrier(std::uint64_t bytes) const {
    return role_ == Role::initiator ? Barrier{local_revision_, remote_revision_, bytes}
                                    : Barrier{remote_revision_, local_revision_, bytes};
}
Result<bool> PauseState::current(Barrier b) const {
    const auto expected = barrier(b.bytes);
    if (b.sender_revision > expected.sender_revision ||
        b.receiver_revision > expected.receiver_revision)
        return std::unexpected(Error{ErrorCode::invalid_state});
    return b.sender_revision == expected.sender_revision &&
           b.receiver_revision == expected.receiver_revision;
}
void PauseState::acknowledge() {
    acknowledged_ = true;
    if (remote_)
        remote_confirmed_ = true;
}
} // namespace bridge
