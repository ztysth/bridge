#include "bridge/io/folder.hpp"
#include "bridge/io/path.hpp"
#include "bridge/security/hash.hpp"
#include "filesystem.hpp"
#include <algorithm>
#include <array>
#include <map>
#include <utility>
namespace bridge::io {
namespace {
using native::File;
using native::Handle;
using native::Metadata;
using native::normalized;
using native::open_root;
using native::stable;
using native::sync;
Result<File> directory_at(Handle parent, const std::string& name) {
    return native::open_at(parent, name, native::Access::read, native::Kind::directory);
}
Result<std::vector<native::Entry>> list(Handle file, std::size_t& count, std::stop_token stop) {
    return native::list(file, count, maximum_folder_entries, stop);
}
std::string folded(std::string value) {
    for (auto& c : value)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return value;
}
Result<void> safe_path(const std::string& name) {
    if (!validate_relative_path(name) || name.size() > maximum_folder_path)
        return std::unexpected(Error{ErrorCode::invalid_path});
    std::size_t begin = 0, depth = 0;
    do {
        const auto end = name.find('/', begin);
        const auto component = name.substr(begin, end == std::string::npos ? end : end - begin);
        if (++depth > maximum_folder_depth || folded(component).starts_with(".bridge-"))
            return std::unexpected(Error{ErrorCode::invalid_path});
        if (end == std::string::npos)
            break;
        begin = end + 1;
    } while (true);
    return {};
}
std::array<std::uint8_t, 11> header(std::uint8_t type, std::size_t length, std::uint64_t size) {
    std::array<std::uint8_t, 11> out{};
    out[0] = type;
    out[1] = static_cast<std::uint8_t>(length >> 8U);
    out[2] = static_cast<std::uint8_t>(length);
    for (unsigned i = 0; i < 8; ++i)
        out[3 + i] = static_cast<std::uint8_t>(size >> ((7U - i) * 8U));
    return out;
}
std::uint64_t number(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 0;
    for (auto byte : bytes)
        value = (value << 8U) | byte;
    return value;
}
struct Writer {
    Handle fd;
    std::uint64_t bytes = 0;
    Result<void> write(std::span<const std::uint8_t> input) {
        if (input.size() > maximum_file_size - bytes)
            return std::unexpected(Error{ErrorCode::invalid_manifest});
        auto result = native::write_at(fd, input, bytes);
        if (result)
            bytes += input.size();
        return result;
    }
};
Result<void> pack(Handle directory, const std::string& prefix, Writer& writer,
                  std::vector<std::uint8_t>& buffer, std::map<std::string, bool>& names,
                  std::size_t& count, std::stop_token stop) {
    auto before = native::metadata(directory);
    if (!before)
        return std::unexpected(before.error());
    auto entries = list(directory, count, stop);
    if (!entries)
        return std::unexpected(entries.error());
    for (const auto& entry : *entries) {
        if (stop.stop_requested())
            return std::unexpected(Error{ErrorCode::cancelled});
        const auto path = prefix.empty() ? entry.name : prefix + '/' + entry.name;
        auto safe = safe_path(path);
        if (!safe)
            return safe;
        const bool is_directory = entry.info.kind == native::Kind::directory;
        if ((!is_directory && entry.info.kind != native::Kind::regular))
            return std::unexpected(Error{ErrorCode::invalid_path});
        auto key = path_collision_key(path);
        if (!key)
            return std::unexpected(key.error());
        if (!names.emplace(std::move(*key), is_directory).second)
            return std::unexpected(Error{ErrorCode::destination_conflict});
        auto child =
            native::open_at(directory, entry.name, native::Access::read,
                            is_directory ? native::Kind::directory : native::Kind::regular);
        if (!child)
            return std::unexpected(child.error());
        auto unchanged = stable(child->get(), entry.info);
        if (!unchanged)
            return unchanged;
        const auto size = is_directory ? 0 : static_cast<std::uint64_t>(entry.info.size);
        if (size > maximum_file_size - writer.bytes)
            return std::unexpected(Error{ErrorCode::invalid_manifest});
        auto written = writer.write(header(is_directory ? 1 : 2, path.size(), size));
        if (!written)
            return written;
        written = writer.write(
            std::span(reinterpret_cast<const std::uint8_t*>(path.data()), path.size()));
        if (!written)
            return written;
        if (is_directory) {
            auto result = pack(child->get(), path, writer, buffer, names, count, stop);
            if (!result)
                return result;
        } else {
            for (std::uint64_t offset = 0; offset < size;) {
                if (stop.stop_requested())
                    return std::unexpected(Error{ErrorCode::cancelled});
                const auto amount =
                    static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - offset));
                auto read = native::read_at(child->get(), std::span(buffer).first(amount), offset,
                                            ErrorCode::source_changed, stop);
                if (!read)
                    return read;
                written = writer.write(std::span(buffer).first(amount));
                if (!written)
                    return written;
                offset += amount;
            }
            unchanged = stable(child->get(), entry.info);
            if (!unchanged)
                return unchanged;
        }
    }
    return stable(directory, *before);
}
struct Snapshot {
    std::filesystem::path directory;
    ~Snapshot() {
        if (!directory.empty()) {
            std::error_code ignored;
            std::filesystem::remove(directory / "payload", ignored);
            std::filesystem::remove(directory, ignored);
        }
    }
};
Result<void> collision(Handle root, const std::string& name) {
    std::size_t count = 0;
    // Destination enumeration is bounded too: avoid arbitrary work on a huge root.
    auto entries = list(root, count, {});
    if (!entries)
        return std::unexpected(entries.error());
    auto key = path_collision_key(name);
    if (!key)
        return std::unexpected(key.error());
    for (const auto& entry : *entries) {
        auto existing = path_collision_key(entry.name);
        // A nonportable local name cannot equal an accepted name under NFC/folding.
        // Do not reject an unrelated existing file merely for its spelling.
        if (existing && *existing == *key)
            return std::unexpected(Error{ErrorCode::destination_conflict});
    }
    return {};
}
std::string state_name(const TransferId& id) {
    std::string name = ".bridge-";
    constexpr std::string_view hex = "0123456789abcdef";
    for (auto byte : id) {
        name.push_back(hex[byte >> 4U]);
        name.push_back(hex[byte & 15U]);
    }
    return name + ".folder-state";
}
Result<File> beneath(Handle root, const std::string& path) {
    auto copy = native::duplicate(root);
    if (!copy)
        return std::unexpected(copy.error());
    File current = std::move(*copy);
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        auto next = directory_at(current.get(),
                                 path.substr(begin, end == std::string::npos ? end : end - begin));
        if (!next)
            return std::unexpected(next.error());
        current = std::move(*next);
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return current;
}
// Only called beneath our exclusively locked, mode-0700 private namespace.
Result<void> remove_stage(Handle parent, const std::string& name, unsigned depth = 0) {
    if (depth > maximum_folder_depth)
        return std::unexpected(Error{ErrorCode::invalid_path});
    auto info = native::child_metadata(parent, name);
    if (!info)
        return std::unexpected(info.error());
    if (!info->has_value())
        return {};
    if ((**info).kind == native::Kind::directory) {
        auto dir = directory_at(parent, name);
        if (!dir)
            return std::unexpected(dir.error());
        std::size_t count = 0;
        auto entries = list(dir->get(), count, {});
        if (!entries)
            return std::unexpected(entries.error());
        for (const auto& entry : *entries) {
            auto removed = remove_stage(dir->get(), entry.name, depth + 1);
            if (!removed)
                return removed;
        }
    }
    return native::remove(parent, name);
}
struct Reader {
    Handle fd;
    std::uint64_t remaining;
    security::Sha256 digest;
    std::uint64_t offset = 0;
    Result<void> read(std::span<std::uint8_t> output, std::stop_token stop) {
        if (output.size() > remaining)
            return std::unexpected(Error{ErrorCode::malformed_frame});
        auto read = native::read_at(fd, output, offset, ErrorCode::malformed_frame, stop);
        if (!read)
            return read;
        auto updated = digest.update(output);
        if (!updated)
            return updated;
        remaining -= output.size();
        offset += output.size();
        return {};
    }
};
Result<void> extract(Reader& reader, Handle stage, std::stop_token stop) {
    std::array<std::uint8_t, 8> magic{};
    auto read = reader.read(magic, stop);
    constexpr std::array<std::uint8_t, 8> expected{'B', 'R', 'F', 'O', 'L', 'D', '0', '1'};
    if (!read)
        return read;
    if (magic != expected)
        return std::unexpected(Error{ErrorCode::malformed_frame});
    std::map<std::string, std::pair<std::string, bool>> names;
    std::vector<std::string> directories;
    std::vector<std::uint8_t> buffer(chunk_size);
    while (true) {
        std::array<std::uint8_t, 11> bytes{};
        read = reader.read(bytes, stop);
        if (!read)
            return read;
        const auto path_length = number(std::span(bytes).subspan(1, 2));
        const auto length = number(std::span(bytes).subspan(3, 8));
        if (bytes[0] == 0) {
            if (path_length != 0 || length != 0 || reader.remaining != 0)
                return std::unexpected(Error{ErrorCode::malformed_frame});
            break;
        }
        if ((bytes[0] != 1 && bytes[0] != 2) || path_length == 0 ||
            path_length > maximum_folder_path || length > maximum_file_size ||
            (bytes[0] == 1 && length != 0) || names.size() >= maximum_folder_entries)
            return std::unexpected(Error{ErrorCode::invalid_manifest});
        std::string path(static_cast<std::size_t>(path_length), '\0');
        read =
            reader.read(std::span(reinterpret_cast<std::uint8_t*>(path.data()), path.size()), stop);
        if (!read)
            return read;
        auto safe = safe_path(path);
        if (!safe)
            return safe;
        auto key = path_collision_key(path);
        if (!key)
            return std::unexpected(key.error());
        if (!names.emplace(std::move(*key), std::pair{path, bytes[0] == 1}).second)
            return std::unexpected(Error{ErrorCode::destination_conflict});
        const auto slash = path.rfind('/');
        const auto parent_path = slash == std::string::npos ? std::string{} : path.substr(0, slash);
        if (!parent_path.empty()) {
            auto parent_key = path_collision_key(parent_path);
            if (!parent_key)
                return std::unexpected(parent_key.error());
            const auto found = names.find(*parent_key);
            if (found == names.end() || !found->second.second || found->second.first != parent_path)
                return std::unexpected(Error{ErrorCode::invalid_path});
        }
        auto parent = beneath(stage, parent_path);
        if (!parent)
            return std::unexpected(parent.error());
        const auto leaf = slash == std::string::npos ? path : path.substr(slash + 1);
        if (bytes[0] == 1) {
            auto created = native::open_at(parent->get(), leaf, native::Access::create_directory);
            if (!created)
                return std::unexpected(created.error());
            directories.push_back(path);
        } else {
            if (length > reader.remaining)
                return std::unexpected(Error{ErrorCode::malformed_frame});
            auto file = native::open_at(parent->get(), leaf, native::Access::create_file);
            if (!file)
                return std::unexpected(file.error());
            Writer writer{file->get()};
            for (std::uint64_t left = length; left > 0;) {
                auto bytes_to_read = std::span(buffer).first(
                    static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), left)));
                read = reader.read(bytes_to_read, stop);
                if (!read)
                    return read;
                auto written = writer.write(bytes_to_read);
                if (!written)
                    return written;
                left -= bytes_to_read.size();
            }
            auto synced = sync(file->get());
            if (!synced)
                return synced;
        }
    }
    for (auto i = directories.rbegin(); i != directories.rend(); ++i) {
        if (stop.stop_requested())
            return std::unexpected(Error{ErrorCode::cancelled});
        auto dir = beneath(stage, *i);
        if (!dir)
            return std::unexpected(dir.error());
        auto synced = sync(dir->get());
        if (!synced)
            return synced;
    }
    return sync(stage);
}
} // namespace
struct SourcePayload::Impl {
    std::unique_ptr<Snapshot> snapshot;
    SourceFile file;
    FileManifest description;
    Impl(std::unique_ptr<Snapshot> storage, SourceFile source, FileManifest manifest)
        : snapshot(std::move(storage)), file(std::move(source)), description(std::move(manifest)) {}
};
SourcePayload::SourcePayload(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SourcePayload::~SourcePayload() = default;
SourcePayload::SourcePayload(SourcePayload&&) noexcept = default;
SourcePayload& SourcePayload::operator=(SourcePayload&&) noexcept = default;
const FileManifest& SourcePayload::manifest() const { return impl_->description; }
Result<Chunk> SourcePayload::read(std::uint64_t offset, std::stop_token stop) {
    return impl_->file.read(offset, stop);
}
Result<SourcePayload> SourcePayload::open(const std::filesystem::path& selected, TransferId id,
                                          std::stop_token stop) {
    if (stop.stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    const auto path = normalized(selected);
    if (path.empty() || path.native().find('\0') != std::string::npos)
        return std::unexpected(Error{ErrorCode::invalid_path});
    auto opened = native::open_path(path, native::Kind::other);
    if (!opened)
        return std::unexpected(opened.error());
    auto info = native::metadata(opened->get());
    if (!info)
        return std::unexpected(info.error());
    if (info->kind == native::Kind::regular) {
        auto file = SourceFile::open(path, id, stop);
        if (!file)
            return std::unexpected(file.error());
        auto manifest = file->manifest();
        return SourcePayload(
            std::make_unique<Impl>(nullptr, std::move(*file), std::move(manifest)));
    }
    auto name = native::filename(path);
    if (!name)
        return std::unexpected(name.error());
    if (info->kind != native::Kind::directory || !safe_path(*name))
        return std::unexpected(Error{ErrorCode::invalid_path});
    auto root = open_root(path);
    if (!root)
        return std::unexpected(root.error());
    auto unchanged = stable(root->get(), *info);
    if (!unchanged)
        return std::unexpected(unchanged.error());
    auto temporary = native::temporary_directory();
    if (!temporary)
        return std::unexpected(temporary.error());
    auto snapshot = std::make_unique<Snapshot>();
    snapshot->directory = *temporary;
    auto snapshot_root = open_root(snapshot->directory);
    if (!snapshot_root)
        return std::unexpected(snapshot_root.error());
    auto output = native::open_at(snapshot_root->get(), "payload", native::Access::create_file);
    if (!output)
        return std::unexpected(output.error());
    Writer writer{output->get()};
    constexpr std::array<std::uint8_t, 8> magic{'B', 'R', 'F', 'O', 'L', 'D', '0', '1'};
    auto written = writer.write(magic);
    if (!written)
        return std::unexpected(written.error());
    std::vector<std::uint8_t> buffer(chunk_size);
    std::map<std::string, bool> names;
    std::size_t count = 0;
    auto packed = pack(root->get(), {}, writer, buffer, names, count, stop);
    if (!packed)
        return std::unexpected(packed.error());
    written = writer.write(header(0, 0, 0));
    if (!written)
        return std::unexpected(written.error());
    unchanged = stable(root->get(), *info);
    if (!unchanged)
        return std::unexpected(unchanged.error());
    auto file = SourceFile::open(snapshot->directory / "payload", id, stop);
    if (!file)
        return std::unexpected(file.error());
    auto manifest = file->manifest();
    manifest.name = *name;
    manifest.kind = PayloadKind::folder;
    return SourcePayload(
        std::make_unique<Impl>(std::move(snapshot), std::move(*file), std::move(manifest)));
}
struct FolderDestination::Impl {
    File root, state;
    std::filesystem::path payload;
    FileManifest manifest;
};
FolderDestination::FolderDestination(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FolderDestination::~FolderDestination() = default;
FolderDestination::FolderDestination(FolderDestination&&) noexcept = default;
FolderDestination& FolderDestination::operator=(FolderDestination&&) noexcept = default;
const std::filesystem::path& FolderDestination::payload_root() const { return impl_->payload; }
Result<FolderDestination> FolderDestination::open(const std::filesystem::path& selected,
                                                  const FileManifest& m, bool resume) {
    if (!validate_manifest(m) || m.kind != PayloadKind::folder || !safe_path(m.name) ||
        m.name.find('/') != std::string::npos)
        return std::unexpected(Error{ErrorCode::invalid_manifest});
    auto root = open_root(selected);
    if (!root)
        return std::unexpected(root.error());
    auto compatible = native::receive_root(root->get());
    if (!compatible)
        return std::unexpected(compatible.error());
    auto conflict = collision(root->get(), m.name);
    if (!conflict)
        return std::unexpected(conflict.error());
    const auto name = state_name(m.id);
    auto state = native::open_at(root->get(), name,
                                 resume ? native::Access::read : native::Access::create_directory,
                                 native::Kind::directory);
    if (!state)
        return std::unexpected(state.error());
    auto private_state = native::validate_private(state->get());
    if (!private_state)
        return std::unexpected(private_state.error());
    auto locked = native::lock(*state);
    if (!locked)
        return std::unexpected(locked.error());
    auto synced = sync(root->get());
    if (!synced)
        return std::unexpected(synced.error());
    auto impl = std::make_unique<Impl>();
    impl->root = std::move(*root);
    impl->state = std::move(*state);
    impl->payload = normalized(selected) / name;
    impl->manifest = m;
    return FolderDestination(std::move(impl));
}
Result<void> FolderDestination::publish(std::stop_token stop) {
    if (stop.stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    auto conflict = collision(impl_->root.get(), impl_->manifest.name);
    if (!conflict)
        return conflict;
    auto bundle = native::open_at(impl_->state.get(), impl_->manifest.name, native::Access::read);
    if (!bundle)
        return std::unexpected(bundle.error());
    auto info = native::metadata(bundle->get());
    if (!info)
        return std::unexpected(info.error());
    if (info->kind != native::Kind::regular || info->links != 1 ||
        info->size != impl_->manifest.size)
        return std::unexpected(Error{ErrorCode::invalid_checkpoint});
    auto removed = remove_stage(impl_->state.get(), ".bridge-extract");
    if (!removed)
        return removed;
    auto stage =
        native::open_at(impl_->state.get(), ".bridge-extract", native::Access::create_directory);
    if (!stage)
        return std::unexpected(stage.error());
    auto digest = security::Sha256::create();
    if (!digest)
        return std::unexpected(digest.error());
    Reader reader{bundle->get(), impl_->manifest.size, std::move(*digest)};
    auto extracted = extract(reader, stage->get(), stop);
    if (!extracted)
        return extracted;
    auto hash = reader.digest.finish();
    if (!hash)
        return std::unexpected(hash.error());
    if (*hash != impl_->manifest.digest)
        return std::unexpected(Error{ErrorCode::checksum_mismatch});
    auto unchanged = stable(bundle->get(), *info);
    if (!unchanged)
        return unchanged;
    if (stop.stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    conflict = collision(impl_->root.get(), impl_->manifest.name);
    if (!conflict)
        return conflict;
    auto published = native::rename_directory(stage->get(), impl_->state.get(), impl_->root.get(),
                                              impl_->manifest.name);
    if (!published)
        return published;
    return sync(impl_->state.get());
}
} // namespace bridge::io
