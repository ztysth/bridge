#include "bridge/io/path.hpp"
#include <string>
#include <utf8proc.h>
#include <utility>
#include <vector>
namespace bridge::io {
static_assert(UTF8PROC_VERSION_MAJOR > 2 ||
                  (UTF8PROC_VERSION_MAJOR == 2 && UTF8PROC_VERSION_MINOR >= 9),
              "bridge requires utf8proc 2.9 or newer");
namespace {
constexpr utf8proc_ssize_t maximum_key_size = 4096;
bool display_control(utf8proc_int32_t c) {
    const auto category = utf8proc_category(c);
    return category == UTF8PROC_CATEGORY_CC || category == UTF8PROC_CATEGORY_ZL ||
           category == UTF8PROC_CATEGORY_ZP || c == 0x061c || c == 0x200e || c == 0x200f ||
           (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x206f);
}
bool device_name(std::string base) {
    base.resize(base.find('.') == std::string::npos ? base.size() : base.find('.'));
    while (!base.empty() && base.back() == ' ')
        base.pop_back();
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL")
        return true;
    if (!base.starts_with("COM") && !base.starts_with("LPT"))
        return false;
    const std::string_view number(base.data() + 3, base.size() - 3);
    return (number.size() == 1 && number.front() >= '1' && number.front() <= '9') ||
           number == "\xC2\xB9" || number == "\xC2\xB2" || number == "\xC2\xB3";
}
} // namespace
Result<void> validate_relative_path(std::string_view path) {
    const auto invalid = std::unexpected(Error{ErrorCode::invalid_path});
    if (path.empty() || path.size() > 4096 || path.front() == '/' || path.back() == '/')
        return invalid;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        const auto part =
            path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
        if (part.empty() || part.size() > 255 || part == "." || part == ".." ||
            part.back() == '.' || part.back() == ' ')
            return invalid;
        for (std::size_t offset = 0; offset < part.size();) {
            utf8proc_int32_t c = 0;
            const auto length =
                utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(part.data() + offset),
                                 static_cast<utf8proc_ssize_t>(part.size() - offset), &c);
            if (length <= 0 || display_control(c))
                return invalid;
            offset += static_cast<std::size_t>(length);
        }
        std::string base;
        for (const char character : part) {
            const auto c = static_cast<unsigned char>(character);
            if (c == 92 ||
                std::string_view("<>:\"|?*").find(static_cast<char>(c)) != std::string_view::npos)
                return invalid;
            base.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A')
                                                : static_cast<char>(c));
        }
        if (device_name(std::move(base)))
            return invalid;
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return {};
}
Result<std::string> path_collision_key(std::string_view path) {
    auto valid = validate_relative_path(path);
    if (!valid)
        return std::unexpected(valid.error());
    // utf8proc's enum is a documented bitmask; combined values are valid options.
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
    constexpr auto options =
        static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD);
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto* bytes = reinterpret_cast<const utf8proc_uint8_t*>(path.data());
    const auto size = static_cast<utf8proc_ssize_t>(path.size());
    const auto count = utf8proc_decompose(bytes, size, nullptr, 0, options);
    if (count <= 0 || count > maximum_key_size)
        return std::unexpected(Error{ErrorCode::invalid_path});
    // Reencoding is in-place; an extra UTF-32 slot leaves room for the terminator.
    std::vector<utf8proc_int32_t> buffer(static_cast<std::size_t>(count) + 1);
    if (utf8proc_decompose(bytes, size, buffer.data(), count, options) != count)
        return std::unexpected(Error{ErrorCode::invalid_path});
    const auto length = utf8proc_reencode(buffer.data(), count, options);
    if (length <= 0 || length > maximum_key_size)
        return std::unexpected(Error{ErrorCode::invalid_path});
    return std::string(reinterpret_cast<const char*>(buffer.data()),
                       static_cast<std::size_t>(length));
}
} // namespace bridge::io
