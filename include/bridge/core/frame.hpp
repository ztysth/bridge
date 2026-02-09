#pragma once
#include "bridge/core/error.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace bridge {
inline constexpr std::size_t header_size = 12;
inline constexpr std::uint32_t max_control_payload = 4096;
inline constexpr std::uint32_t max_file_payload = 1048576 + 60;
inline constexpr std::array<std::uint8_t, 16> ping_payload{'b', 'r', 'i', 'd', 'g', 'e', '-', 's',
                                                           'e', 's', 's', 'i', 'o', 'n', '-', '1'};
enum class Message : std::uint8_t {
    confirm = 1,
    hello,
    hello_ack,
    ping,
    pong,
    close,
    close_ack,
    cancel,
    error,
    offer = 32,
    accept_prefix,
    data_chunk,
    durable_ack,
    hold,
    pause_barrier,
    barrier_ack,
    finish_file,
    file_committed,
    heartbeat
};
constexpr bool is_file_message(Message type) {
    return type >= Message::offer && type <= Message::heartbeat;
}
struct Header {
    Message type;
    std::uint32_t length;
};
struct Frame {
    Message type;
    std::vector<std::uint8_t> payload;
};
Result<void> validate_payload(Message type, std::span<const std::uint8_t> payload);
Result<Header> decode_header(std::span<const std::uint8_t> bytes);
Result<Frame> decode_frame(std::span<const std::uint8_t> bytes);
Result<std::vector<std::uint8_t>> encode_frame(Message type,
                                               std::span<const std::uint8_t> payload = {});
std::uint16_t read_u16(std::span<const std::uint8_t, 2> bytes);
} // namespace bridge
