#include "bridge/io/path.hpp"
#include <cstddef>
#include <cstdint>
#include <string_view>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 4096)
        return 0;
    const std::string_view path(reinterpret_cast<const char*>(data), size);
    const auto valid = bridge::io::validate_relative_path(path);
    const auto key = bridge::io::path_collision_key(path);
    if (key && (!valid || key->empty() || key->size() > 4096))
        __builtin_trap();
    return 0;
}
