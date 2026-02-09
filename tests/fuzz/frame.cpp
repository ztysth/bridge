#include "bridge/core/file_protocol.hpp"
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);
    if (size == bridge::header_size)
        static_cast<void>(bridge::decode_header(input));
    static_cast<void>(bridge::decode_frame(input));
    static_cast<void>(bridge::decode_offer(input));
    static_cast<void>(bridge::decode_prefix(input));
    static_cast<void>(bridge::decode_hold(input));
    static_cast<void>(bridge::decode_barrier(input));
    const bridge::FileManifest manifest{{1}, "fuzz.bin", 3, {}};
    static_cast<void>(bridge::decode_chunk(input, manifest));
    return 0;
}
