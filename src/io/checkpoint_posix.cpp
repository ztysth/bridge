#include "bridge/core/diagnostics.hpp"
#include "bridge/io/checkpoint.hpp"
#include "bridge/io/path.hpp"
#include "bridge/security/hash.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
namespace bridge::io {
namespace {
static_assert(sizeof(off_t) >= 8, "Checkpoint IO requires 64-bit filesystem offsets");
constexpr std::size_t header_prefix_size = 68;
constexpr std::size_t record_size = 80;
class Descriptor {
  public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() {
        // Do not retry close on EINTR: some kernels have already released the fd.
        if (value_ >= 0)
            ::close(value_);
    }
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        Descriptor temporary(std::move(other));
        std::swap(value_, temporary.value_);
        return *this;
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return value_; }

  private:
    int value_;
};
Error system_error(int code = errno) {
    switch (code) {
    case ENOSPC:
    case EDQUOT:
        return {ErrorCode::disk_full, code};
    case EACCES:
    case EPERM:
        return {ErrorCode::permission_denied, code};
    case EEXIST:
        return {ErrorCode::destination_conflict, code};
    case ELOOP:
        return {ErrorCode::invalid_path, code};
    default:
        return {ErrorCode::io_failed, code};
    }
}
Result<void> sync(int descriptor) {
    int result = 0;
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    if (result != 0)
        return std::unexpected(system_error());
    return {};
}
Result<void> read_at(int fd, std::span<std::uint8_t> output, std::uint64_t offset) {
    while (!output.empty()) {
        const auto count = ::pread(fd, output.data(), output.size(), static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return std::unexpected(system_error());
        if (count == 0)
            return std::unexpected(Error{ErrorCode::invalid_checkpoint});
        output = output.subspan(static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
    return {};
}
Result<void> write_at(int fd, std::span<const std::uint8_t> input, std::uint64_t offset) {
    while (!input.empty()) {
        const auto count = ::pwrite(fd, input.data(), input.size(), static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return std::unexpected(count < 0 ? system_error() : Error{ErrorCode::io_failed});
        input = input.subspan(static_cast<std::size_t>(count));
        offset += static_cast<std::uint64_t>(count);
    }
    return {};
}
Result<struct stat> metadata(int fd) {
    struct stat value {};
    if (::fstat(fd, &value) != 0)
        return std::unexpected(system_error());
    if (!S_ISREG(value.st_mode) || value.st_size < 0)
        return std::unexpected(Error{ErrorCode::invalid_checkpoint});
    return value;
}
Result<Descriptor> open_regular(int root, const std::string& name, int flags) {
    Descriptor fd(::openat(root, name.c_str(), flags | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    if (fd.get() < 0)
        return std::unexpected(system_error());
    auto info = metadata(fd.get());
    if (!info)
        return std::unexpected(info.error());
    return fd;
}
Result<Descriptor> open_root(const std::filesystem::path& path) {
    if (path.empty() || path.native().find('\0') != std::string::npos)
        return std::unexpected(Error{ErrorCode::invalid_path});
    auto selected = path;
    // A trailing slash makes open resolve a symlink as an intermediate component,
    // bypassing O_NOFOLLOW. Strip empty final components, preserving filesystem /.
    while (selected.has_relative_path() && selected.filename().empty())
        selected = selected.parent_path();
    Descriptor root(::open(selected.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (root.get() < 0)
        return std::unexpected(system_error());
    return root;
}
Result<void> validate(const FileManifest& manifest) {
    auto valid = validate_manifest(manifest);
    if (!valid)
        return valid;
    valid = validate_relative_path(manifest.name);
    if (!valid)
        return valid;
    std::string lowered = manifest.name;
    for (auto& c : lowered)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    if (manifest.name.find('/') != std::string::npos || lowered.starts_with(".bridge-"))
        return std::unexpected(Error{ErrorCode::invalid_path});
    return {};
}
void put(std::vector<std::uint8_t>& output, std::uint64_t value, unsigned width) {
    for (unsigned remaining = width; remaining > 0; --remaining)
        output.push_back(static_cast<std::uint8_t>(value >> ((remaining - 1U) * 8U)));
}
std::uint64_t get(std::span<const std::uint8_t> input) {
    std::uint64_t value = 0;
    for (auto byte : input)
        value = (value << 8U) | byte;
    return value;
}
Result<void> seal(std::vector<std::uint8_t>& output) {
    auto digest = security::sha256(output);
    if (!digest)
        return std::unexpected(digest.error());
    output.insert(output.end(), digest->begin(), digest->end());
    return {};
}
Result<std::vector<std::uint8_t>> encode_header(const FileManifest& manifest) {
    std::vector<std::uint8_t> bytes{'B', 'R', 'C', 'P'};
    bytes.reserve(header_prefix_size + manifest.name.size() + 32);
    put(bytes, 1, 2);
    put(bytes, manifest.name.size(), 2);
    bytes.insert(bytes.end(), manifest.id.begin(), manifest.id.end());
    put(bytes, manifest.size, 8);
    put(bytes, chunk_size, 4);
    bytes.insert(bytes.end(), manifest.digest.begin(), manifest.digest.end());
    bytes.insert(bytes.end(), manifest.name.begin(), manifest.name.end());
    auto sealed = seal(bytes);
    if (!sealed)
        return std::unexpected(sealed.error());
    return bytes;
}
Result<std::array<std::uint8_t, record_size>>
encode_record(std::uint64_t index, std::uint32_t length, const Digest& digest) {
    std::array<std::uint8_t, record_size> bytes{'B', 'R', 'C', 'K'};
    for (unsigned offset = 0; offset < 8; ++offset)
        bytes[4 + offset] = static_cast<std::uint8_t>(index >> ((7U - offset) * 8U));
    for (unsigned offset = 0; offset < 4; ++offset)
        bytes[12 + offset] = static_cast<std::uint8_t>(length >> ((3U - offset) * 8U));
    std::ranges::copy(digest, bytes.begin() + 16);
    auto checksum = security::sha256(std::span(bytes).first(48));
    if (!checksum)
        return std::unexpected(checksum.error());
    std::ranges::copy(*checksum, bytes.begin() + 48);
    return bytes;
}
std::string stem(const TransferId& id) {
    std::string name = ".bridge-";
    constexpr std::string_view hex = "0123456789abcdef";
    for (auto byte : id) {
        name.push_back(hex[byte >> 4U]);
        name.push_back(hex[byte & 15U]);
    }
    return name;
}
bool same_file(const struct stat& first, const struct stat& second) {
    return first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}
} // namespace
struct PartialFile::Impl {
    FileManifest manifest;
    Descriptor root, journal, partial;
    std::string partial_name, journal_name;
    std::uint64_t durable = 0;
    std::uint64_t journal_end = 0;
    bool poisoned = false, published = false, complete = false, partial_named = true;
    CheckpointInjection injection;
    Failpoint trigger;
    Impl(FileManifest value, Descriptor directory, CheckpointInjection fault)
        : manifest(std::move(value)), root(std::move(directory)),
          partial_name(stem(manifest.id) + ".part"), journal_name(stem(manifest.id) + ".journal"),
          injection(fault), trigger(fault.hit) {}
    Result<void> fault(CheckpointFault point) {
        if (injection.point == point && trigger.hit()) {
            poisoned = true;
            return std::unexpected(Error{ErrorCode::injected_failure});
        }
        return {};
    }
    Result<void> storage(Result<void> result) {
        if (!result)
            poisoned = true;
        return result;
    }
    Result<void> lock() {
        auto info = metadata(journal.get());
        if (!info)
            return std::unexpected(info.error());
        if (info->st_nlink != 1)
            return std::unexpected(Error{ErrorCode::invalid_checkpoint});
        if (::flock(journal.get(), LOCK_EX | LOCK_NB) != 0)
            return std::unexpected(errno == EWOULDBLOCK ? Error{ErrorCode::checkpoint_busy}
                                                        : system_error());
        return {};
    }
    Result<void> cleanup() {
        auto synced = storage(sync(root.get()));
        if (!synced)
            return synced;
        if (partial_named) {
            if (::unlinkat(root.get(), partial_name.c_str(), 0) != 0)
                return storage(std::unexpected(system_error()));
            partial_named = false;
            synced = storage(sync(root.get()));
            if (!synced)
                return synced;
        }
        if (::unlinkat(root.get(), journal_name.c_str(), 0) != 0)
            return storage(std::unexpected(system_error()));
        synced = storage(sync(root.get()));
        if (!synced)
            return synced;
        complete = true;
        return {};
    }
    Result<Digest> hash_file(std::stop_token stop) {
        auto info = metadata(partial.get());
        if (!info)
            return std::unexpected(info.error());
        if (static_cast<std::uint64_t>(info->st_size) != manifest.size)
            return std::unexpected(Error{ErrorCode::invalid_checkpoint});
        auto hash = security::Sha256::create();
        if (!hash)
            return std::unexpected(hash.error());
        std::vector<std::uint8_t> buffer(chunk_size);
        for (std::uint64_t offset = 0; offset < manifest.size;) {
            if (stop.stop_requested())
                return std::unexpected(Error{ErrorCode::cancelled});
            auto bytes = std::span(buffer).first(static_cast<std::size_t>(
                std::min<std::uint64_t>(chunk_size, manifest.size - offset)));
            auto read = read_at(partial.get(), bytes, offset);
            if (!read)
                return std::unexpected(read.error());
            auto updated = hash->update(bytes);
            if (!updated)
                return std::unexpected(updated.error());
            offset += bytes.size();
        }
        return hash->finish();
    }
    Result<void> recover(std::stop_token stop) {
        auto expected = encode_header(manifest);
        auto info = metadata(journal.get());
        if (!expected)
            return std::unexpected(expected.error());
        if (!info)
            return std::unexpected(info.error());
        const auto disk_size = static_cast<std::uint64_t>(info->st_size);
        const auto header_size = expected->size();
        if (disk_size < header_size ||
            disk_size > header_size + chunk_count(manifest.size) * record_size + record_size - 1)
            return std::unexpected(Error{ErrorCode::invalid_checkpoint});
        std::vector<std::uint8_t> header(header_prefix_size);
        auto read = read_at(journal.get(), header, 0);
        if (!read)
            return read;
        const auto name_length = get(std::span(header).subspan(6, 2));
        if (name_length == 0 || name_length > 255 || name_length != manifest.name.size())
            return std::unexpected(Error{ErrorCode::invalid_checkpoint});
        header.resize(header_size);
        read = read_at(journal.get(), header, 0);
        if (!read)
            return read;
        if (header != *expected)
            return std::unexpected(Error{ErrorCode::invalid_checkpoint});

        struct stat final_info {};
        const bool final_exists =
            ::fstatat(root.get(), manifest.name.c_str(), &final_info, AT_SYMLINK_NOFOLLOW) == 0;
        if (!final_exists && errno != ENOENT)
            return std::unexpected(system_error());
        auto opened = open_regular(root.get(), partial_name, O_RDWR);
        if (!opened) {
            // Missing partial is legal only during validated publication recovery.
            struct stat unused {};
            if (::fstatat(root.get(), partial_name.c_str(), &unused, AT_SYMLINK_NOFOLLOW) == 0 ||
                errno != ENOENT || !final_exists)
                return std::unexpected(opened.error());
            opened = open_regular(root.get(), manifest.name, O_RDONLY);
            partial_named = false;
        }
        if (!opened)
            return std::unexpected(opened.error());
        partial = std::move(*opened);
        auto partial_info = metadata(partial.get());
        if (!partial_info)
            return std::unexpected(partial_info.error());
        if (final_exists) {
            if (!S_ISREG(final_info.st_mode) || !same_file(*partial_info, final_info))
                return std::unexpected(Error{ErrorCode::destination_conflict});
            published = true;
        }
        const auto expected_links = published && partial_named ? 2U : 1U;
        if (partial_info->st_nlink != expected_links ||
            static_cast<std::uint64_t>(partial_info->st_size) > manifest.size)
            return std::unexpected(Error{ErrorCode::invalid_checkpoint});
        const auto records = (disk_size - header_size) / record_size;
        if (records > chunk_count(manifest.size))
            return std::unexpected(Error{ErrorCode::invalid_checkpoint});
        auto whole_hash = security::Sha256::create();
        if (!whole_hash)
            return std::unexpected(whole_hash.error());
        std::vector<std::uint8_t> buffer(chunk_size);
        for (std::uint64_t index = 0; index < records; ++index) {
            if (stop.stop_requested())
                return std::unexpected(Error{ErrorCode::cancelled});
            std::array<std::uint8_t, record_size> record{};
            read = read_at(journal.get(), record, header_size + index * record_size);
            if (!read)
                return read;
            auto length = chunk_length(manifest.size, index);
            auto checksum = security::sha256(std::span(record).first(48));
            if (!length || !checksum)
                return std::unexpected(!length ? length.error() : checksum.error());
            if (!std::ranges::equal(std::span(record).first(4),
                                    std::array<std::uint8_t, 4>{'B', 'R', 'C', 'K'}) ||
                get(std::span(record).subspan(4, 8)) != index ||
                get(std::span(record).subspan(12, 4)) != *length ||
                !std::ranges::equal(*checksum, std::span(record).subspan(48, 32)))
                return std::unexpected(Error{ErrorCode::invalid_checkpoint});
            auto bytes = std::span(buffer).first(*length);
            read = read_at(partial.get(), bytes, durable);
            if (!read)
                return read;
            auto digest = security::sha256(bytes);
            if (!digest)
                return std::unexpected(digest.error());
            if (!std::ranges::equal(*digest, std::span(record).subspan(16, 32)))
                return std::unexpected(Error{ErrorCode::checksum_mismatch});
            auto updated = whole_hash->update(bytes);
            if (!updated)
                return updated;
            durable += *length;
        }
        journal_end = header_size + records * record_size;
        if (published) {
            auto digest = whole_hash->finish();
            if (!digest)
                return std::unexpected(digest.error());
            if (durable != manifest.size || *digest != manifest.digest ||
                disk_size != journal_end ||
                static_cast<std::uint64_t>(partial_info->st_size) != manifest.size)
                return std::unexpected(Error{ErrorCode::checksum_mismatch});
            return cleanup();
        }
        if (stop.stop_requested())
            return std::unexpected(Error{ErrorCode::cancelled});
        // No mutations until every complete record and acknowledged byte validated.
        if (::ftruncate(journal.get(), static_cast<off_t>(journal_end)) != 0 ||
            ::ftruncate(partial.get(), static_cast<off_t>(durable)) != 0)
            return storage(std::unexpected(system_error()));
        auto synced = storage(sync(partial.get()));
        if (!synced)
            return synced;
        return storage(sync(journal.get()));
    }
};
PartialFile::PartialFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PartialFile::~PartialFile() = default;
PartialFile::PartialFile(PartialFile&&) noexcept = default;
PartialFile& PartialFile::operator=(PartialFile&&) noexcept = default;
Result<PartialFile> PartialFile::create(const std::filesystem::path& root,
                                        const FileManifest& manifest,
                                        CheckpointInjection injection) {
    auto valid = validate(manifest);
    if (!valid)
        return std::unexpected(valid.error());
    auto directory = open_root(root);
    if (!directory)
        return std::unexpected(directory.error());
    auto impl = std::make_unique<Impl>(manifest, std::move(*directory), injection);
    struct stat existing {};
    if (::fstatat(impl->root.get(), manifest.name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
        return std::unexpected(Error{ErrorCode::destination_conflict});
    if (errno != ENOENT)
        return std::unexpected(system_error());
    auto journal = open_regular(impl->root.get(), impl->journal_name, O_RDWR | O_CREAT | O_EXCL);
    if (!journal)
        return std::unexpected(journal.error());
    impl->journal = std::move(*journal);
    auto locked = impl->lock();
    if (!locked)
        return std::unexpected(locked.error());
    auto partial = open_regular(impl->root.get(), impl->partial_name, O_RDWR | O_CREAT | O_EXCL);
    if (!partial)
        return std::unexpected(partial.error());
    impl->partial = std::move(*partial);
    auto header = encode_header(manifest);
    if (!header)
        return std::unexpected(header.error());
    auto written = write_at(impl->journal.get(), *header, 0);
    if (!written)
        return std::unexpected(written.error());
    for (int descriptor : {impl->partial.get(), impl->journal.get(), impl->root.get()}) {
        auto synced = sync(descriptor);
        if (!synced)
            return std::unexpected(synced.error());
    }
    impl->journal_end = header->size();
    return PartialFile(std::move(impl));
}
Result<PartialFile> PartialFile::resume(const std::filesystem::path& root,
                                        const FileManifest& manifest, CheckpointInjection injection,
                                        std::stop_token stop) {
    if (stop.stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    auto valid = validate(manifest);
    if (!valid)
        return std::unexpected(valid.error());
    auto directory = open_root(root);
    if (!directory)
        return std::unexpected(directory.error());
    auto impl = std::make_unique<Impl>(manifest, std::move(*directory), injection);
    auto journal = open_regular(impl->root.get(), impl->journal_name, O_RDWR);
    if (!journal) {
        struct stat journal_info {};
        if (::fstatat(impl->root.get(), impl->journal_name.c_str(), &journal_info,
                      AT_SYMLINK_NOFOLLOW) == 0 ||
            errno != ENOENT)
            return std::unexpected(journal.error());
        // A lost completion notification may follow journal cleanup. Explicit
        // resume may recognize a fully matching final file, without writing it.
        auto final = open_regular(impl->root.get(), manifest.name, O_RDONLY);
        if (!final)
            return std::unexpected(final.error());
        impl->partial = std::move(*final);
        auto info = metadata(impl->partial.get());
        if (!info || info->st_nlink != 1)
            return std::unexpected(info ? Error{ErrorCode::invalid_checkpoint} : info.error());
        auto digest = impl->hash_file(stop);
        if (!digest)
            return std::unexpected(digest.error());
        if (*digest != manifest.digest)
            return std::unexpected(Error{ErrorCode::destination_conflict});
        for (int descriptor : {impl->partial.get(), impl->root.get()}) {
            auto synced = sync(descriptor);
            if (!synced)
                return std::unexpected(synced.error());
        }
        impl->published = true;
        impl->complete = true;
        impl->partial_named = false;
        impl->durable = manifest.size;
        return PartialFile(std::move(impl));
    }
    impl->journal = std::move(*journal);
    auto locked = impl->lock();
    if (!locked)
        return std::unexpected(locked.error());
    auto recovered = impl->recover(stop);
    if (!recovered)
        return std::unexpected(recovered.error());
    return PartialFile(std::move(impl));
}
Result<std::uint64_t> PartialFile::append(std::span<const std::uint8_t> data,
                                          const Digest& chunk_digest, std::stop_token stop) {
    if (stop.stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    if (!impl_ || impl_->poisoned || impl_->published || impl_->complete ||
        impl_->durable == impl_->manifest.size)
        return std::unexpected(Error{ErrorCode::invalid_state});
    auto length = chunk_length(impl_->manifest.size, impl_->durable / chunk_size);
    if (!length || data.size() != *length)
        return std::unexpected(Error{ErrorCode::invalid_manifest});
    auto digest = security::sha256(data);
    if (!digest)
        return std::unexpected(digest.error());
    if (*digest != chunk_digest)
        return std::unexpected(Error{ErrorCode::checksum_mismatch});
    auto record = encode_record(impl_->durable / chunk_size, *length, chunk_digest);
    if (!record)
        return std::unexpected(record.error());
    if ((impl_->injection.point == CheckpointFault::disk_full ||
         impl_->injection.point == CheckpointFault::permission_denied) &&
        impl_->trigger.hit()) {
        impl_->poisoned = true;
        return std::unexpected(Error{impl_->injection.point == CheckpointFault::disk_full
                                         ? ErrorCode::disk_full
                                         : ErrorCode::permission_denied});
    }
    if (impl_->injection.point == CheckpointFault::partial_data_write && impl_->trigger.hit()) {
        auto partial_write = impl_->storage(
            write_at(impl_->partial.get(), data.first(data.size() / 2), impl_->durable));
        impl_->poisoned = true;
        return std::unexpected(partial_write ? Error{ErrorCode::io_failed} : partial_write.error());
    }
    auto written = impl_->storage(write_at(impl_->partial.get(), data, impl_->durable));
    if (!written)
        return std::unexpected(written.error());
    written = impl_->storage(sync(impl_->partial.get()));
    if (!written)
        return std::unexpected(written.error());
    auto fault = impl_->fault(CheckpointFault::after_data_sync);
    if (!fault)
        return std::unexpected(fault.error());
    if (impl_->injection.point == CheckpointFault::torn_record && impl_->trigger.hit()) {
        written = impl_->storage(
            write_at(impl_->journal.get(), std::span(*record).first(40), impl_->journal_end));
        if (!written)
            return std::unexpected(written.error());
        written = impl_->storage(sync(impl_->journal.get()));
        impl_->poisoned = true;
        return std::unexpected(written ? Error{ErrorCode::injected_failure} : written.error());
    }
    written = impl_->storage(write_at(impl_->journal.get(), *record, impl_->journal_end));
    if (!written)
        return std::unexpected(written.error());
    written = impl_->storage(sync(impl_->journal.get()));
    if (!written)
        return std::unexpected(written.error());
    fault = impl_->fault(CheckpointFault::after_record_sync);
    if (!fault)
        return std::unexpected(fault.error());
    impl_->journal_end += record_size;
    impl_->durable += data.size();
    return impl_->durable;
}
Result<void> PartialFile::finish(std::stop_token stop) {
    if (stop.stop_requested())
        return std::unexpected(Error{ErrorCode::cancelled});
    if (!impl_ || impl_->poisoned || impl_->published || impl_->complete ||
        impl_->durable != impl_->manifest.size)
        return std::unexpected(Error{ErrorCode::invalid_state});
    auto digest = impl_->hash_file(stop);
    if (!digest)
        return impl_->storage(std::unexpected(digest.error()));
    if (*digest != impl_->manifest.digest)
        return impl_->storage(std::unexpected(Error{ErrorCode::checksum_mismatch}));
    struct stat named {};
    auto actual = metadata(impl_->partial.get());
    if (!actual)
        return impl_->storage(std::unexpected(actual.error()));
    if (::fstatat(impl_->root.get(), impl_->partial_name.c_str(), &named, AT_SYMLINK_NOFOLLOW) !=
            0 ||
        !S_ISREG(named.st_mode) || !same_file(*actual, named) || actual->st_nlink != 1)
        return impl_->storage(std::unexpected(Error{ErrorCode::invalid_checkpoint}));
    auto synced = impl_->storage(sync(impl_->partial.get()));
    if (!synced)
        return synced;
    if (stop.stop_requested())
        return impl_->storage(std::unexpected(Error{ErrorCode::cancelled}));
    if (::linkat(impl_->root.get(), impl_->partial_name.c_str(), impl_->root.get(),
                 impl_->manifest.name.c_str(), 0) != 0)
        return impl_->storage(std::unexpected(system_error()));
    impl_->published = true;
    auto fault = impl_->fault(CheckpointFault::after_publish);
    if (!fault)
        return fault;
    return impl_->cleanup();
}
std::uint64_t PartialFile::durable_bytes() const { return impl_ ? impl_->durable : 0; }
bool PartialFile::committed() const { return impl_ && impl_->complete; }
} // namespace bridge::io
