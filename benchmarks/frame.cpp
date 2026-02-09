#include "bridge/core/frame.hpp"
#include <chrono>
#include <iostream>
int main() {
    auto encoded = bridge::encode_frame(bridge::Message::ping, bridge::ping_payload);
    if (!encoded)
        return 1;
    constexpr unsigned iterations = 100000;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < iterations; ++i)
        if (!bridge::decode_frame(*encoded))
            return 1;
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    std::cout << "iterations=" << iterations << " elapsed_us=" << elapsed << '\n';
}
