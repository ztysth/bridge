#include "bridge/core/frame.hpp"
#include <algorithm>
namespace bridge {
namespace {
Result<std::pair<std::uint32_t, std::uint32_t>> payload_bounds(Message type) {
    switch (type) {
    case Message::confirm:
    case Message::close:
    case Message::close_ack:
    case Message::cancel:
        return std::pair{0U, 0U};
    case Message::hello:
        return std::pair{4U, 4U};
    case Message::hello_ack:
    case Message::error:
        return std::pair{2U, 2U};
    case Message::ping:
    case Message::pong:
        return std::pair{16U, 16U};
    case Message::offer:
        return std::pair{64U, 318U};
    case Message::data_chunk:
        return std::pair{61U, max_file_payload};
    case Message::accept_prefix:
    case Message::durable_ack:
    case Message::pause_barrier:
    case Message::barrier_ack:
        return std::pair{24U, 24U};
    case Message::hold:
        return std::pair{9U, 9U};
    case Message::finish_file:
    case Message::file_committed:
        return std::pair{16U, 16U};
    case Message::heartbeat:
        return std::pair{0U, 0U};
    }
    return std::unexpected(Error{ErrorCode::malformed_frame});
}
} // namespace
std::uint16_t read_u16(std::span<const std::uint8_t, 2> bytes) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8U) | bytes[1]);
}
Result<Header> decode_header(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != header_size || bytes[0] != 'B' || bytes[1] != 'R' || bytes[2] != 'D' ||
        bytes[3] != 'G' || bytes[7] != 0) {
        return std::unexpected(Error{ErrorCode::malformed_frame});
    }
    if (read_u16(bytes.subspan<4, 2>()) != 1) {
        return std::unexpected(Error{ErrorCode::unsupported_version});
    }
    const auto length = (static_cast<std::uint32_t>(bytes[8]) << 24U) |
                        (static_cast<std::uint32_t>(bytes[9]) << 16U) |
                        (static_cast<std::uint32_t>(bytes[10]) << 8U) | bytes[11];
    const auto type = static_cast<Message>(bytes[6]);
    if (length > (type == Message::data_chunk ? max_file_payload : max_control_payload))
        return std::unexpected(Error{ErrorCode::oversized_frame});
    const auto expected = payload_bounds(type);
    if (!expected || length < expected->first || length > expected->second)
        return std::unexpected(Error{ErrorCode::malformed_frame});
    return Header{type, length};
}
Result<Frame> decode_frame(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < header_size)
        return std::unexpected(Error{ErrorCode::malformed_frame});
    auto header = decode_header(bytes.first(header_size));
    if (!header)
        return std::unexpected(header.error());
    if (bytes.size() != header_size + header->length)
        return std::unexpected(Error{ErrorCode::malformed_frame});
    return Frame{header->type,
                 {bytes.begin() + static_cast<std::ptrdiff_t>(header_size), bytes.end()}};
}
Result<void> validate_payload(Message type, std::span<const std::uint8_t> payload) {
    const auto expected = payload_bounds(type);
    if (!expected || (payload.size() < expected->first || payload.size() > expected->second))
        return std::unexpected(Error{ErrorCode::malformed_frame});
    return {};
}
Result<std::vector<std::uint8_t>> encode_frame(Message type,
                                               std::span<const std::uint8_t> payload) {
    const auto valid = validate_payload(type, payload);
    if (!valid)
        return std::unexpected(valid.error());
    const auto length = static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> result{'B',
                                     'R',
                                     'D',
                                     'G',
                                     0,
                                     1,
                                     static_cast<std::uint8_t>(type),
                                     0,
                                     static_cast<std::uint8_t>(length >> 24U),
                                     static_cast<std::uint8_t>(length >> 16U),
                                     static_cast<std::uint8_t>(length >> 8U),
                                     static_cast<std::uint8_t>(length)};
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}
} // namespace bridge
