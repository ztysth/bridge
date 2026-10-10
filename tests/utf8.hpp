#pragma once
#include <string>
#include <string_view>
namespace bridge::test {
// C++20 char8_t literals avoid the host narrow encoding, including on Windows.
inline std::string utf8(std::u8string_view value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
} // namespace bridge::test
