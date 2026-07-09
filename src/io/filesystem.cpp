#include "filesystem.hpp"
namespace bridge::io::native {
std::filesystem::path normalized(std::filesystem::path path) {
    while (path.has_relative_path() && path.filename().empty())
        path = path.parent_path();
    return path;
}
std::string filename(const std::filesystem::path& path) {
    const auto bytes = path.filename().u8string();
    return {bytes.begin(), bytes.end()};
}
bool same_file(const Metadata& a, const Metadata& b) {
    return a.device == b.device && a.identity == b.identity;
}
bool same_snapshot(const Metadata& a, const Metadata& b) {
    return same_file(a, b) && a.kind == b.kind && a.size == b.size && a.times == b.times;
}
Result<void> stable(Handle file, const Metadata& before) {
    auto after = metadata(file);
    if (!after)
        return std::unexpected(after.error());
    if (!same_snapshot(before, *after))
        return std::unexpected(Error{ErrorCode::source_changed});
    return {};
}
Result<File> open_root(const std::filesystem::path& path) {
    return open_path(normalized(path), Kind::directory);
}
} // namespace bridge::io::native
