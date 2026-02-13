#include "bridge/io/path.hpp"
#include <algorithm>
#include <string>
namespace bridge::io {
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
        std::string base;
        for (const char character : part) {
            const auto c = static_cast<unsigned char>(character);
            if (c < 32 || c > 126 || c == 92 ||
                std::string_view("<>:\"|?*").find(static_cast<char>(c)) != std::string_view::npos)
                return invalid;
            base.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A')
                                                : static_cast<char>(c));
        }
        base.resize(base.find('.') == std::string::npos ? base.size() : base.find('.'));
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
            ((base.starts_with("COM") || base.starts_with("LPT")) && base.size() == 4 &&
             base[3] >= '1' && base[3] <= '9'))
            return invalid;
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return {};
}
} // namespace bridge::io
