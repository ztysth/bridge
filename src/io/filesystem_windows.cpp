#include "filesystem.hpp"
#include <aclapi.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <windows.h>
#include <winternl.h>

// NtCreateFile is the documented Windows handle-relative open API. These two
// companion exports use the SDK's NT types and do not resolve names via paths.
extern "C" NTSYSAPI NTSTATUS NTAPI NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG,
                                                        FILE_INFORMATION_CLASS);
namespace bridge::io::native {
namespace {
constexpr ULONG open_reparse_point = 0x00200000;
constexpr ULONG synchronous_nonalert = 0x00000020;
constexpr ULONG directory_file = 0x00000001;
constexpr ULONG non_directory_file = 0x00000040;
constexpr ULONG disposition_open = 1, disposition_create = 2;
Error system_error(DWORD code = GetLastError()) {
    const auto native = static_cast<int>(code);
    switch (code) {
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return {ErrorCode::disk_full, native};
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return {ErrorCode::permission_denied, native};
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return {ErrorCode::destination_conflict, native};
    case ERROR_CANT_ACCESS_FILE:
    case ERROR_INVALID_NAME:
    case ERROR_DIRECTORY:
        return {ErrorCode::invalid_path, native};
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return {ErrorCode::checkpoint_busy, native};
    default:
        return {ErrorCode::io_failed, native};
    }
}
Error nt_error(NTSTATUS status) { return system_error(RtlNtStatusToDosError(status)); }
bool missing(const Error& error) {
    return error.native_code == ERROR_FILE_NOT_FOUND || error.native_code == ERROR_PATH_NOT_FOUND;
}
Result<std::wstring> wide(const std::string& value) {
    if (value.empty() || value.size() > 4096 || value.find('\0') != std::string::npos)
        return std::unexpected(Error{ErrorCode::invalid_path});
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
        return std::unexpected(Error{ErrorCode::invalid_path});
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), out.data(), size) != size)
        return std::unexpected(system_error());
    return out;
}
Result<std::string> utf8(std::wstring_view value) {
    if (value.size() > 1024)
        return std::unexpected(Error{ErrorCode::invalid_path});
    const int size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return std::unexpected(Error{ErrorCode::invalid_path});
    std::string out(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), out.data(), size, nullptr,
                            nullptr) != size)
        return std::unexpected(system_error());
    return out;
}
// Token buffer owns the SID used by the descriptor; ACL storage is DWORD aligned.
struct PrivateSecurity {
    std::vector<std::uint64_t> token;
    std::vector<DWORD> acl;
    SECURITY_DESCRIPTOR descriptor{};
    Result<void> initialize() {
        HANDLE value = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &value))
            return std::unexpected(system_error());
        File owner(value);
        DWORD size = 0;
        if (GetTokenInformation(owner.get(), TokenUser, nullptr, 0, &size) ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER || size > 65536)
            return std::unexpected(Error{ErrorCode::io_failed});
        token.resize((size + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
        if (!GetTokenInformation(owner.get(), TokenUser, token.data(), size, &size))
            return std::unexpected(system_error());
        const auto sid = reinterpret_cast<const TOKEN_USER*>(token.data())->User.Sid;
        const DWORD acl_size =
            static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) +
            GetLengthSid(sid);
        acl.resize((acl_size + sizeof(DWORD) - 1) / sizeof(DWORD));
        auto* dacl = reinterpret_cast<ACL*>(acl.data());
        if (!InitializeAcl(dacl, acl_size, ACL_REVISION) ||
            !AddAccessAllowedAce(dacl, ACL_REVISION, FILE_ALL_ACCESS, sid) ||
            !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorOwner(&descriptor, sid, FALSE) ||
            !SetSecurityDescriptorDacl(&descriptor, TRUE, dacl, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
            return std::unexpected(system_error());
        return {};
    }
};
Result<File> relative(Handle parent, const std::string& name, ULONG disposition, ACCESS_MASK access,
                      ULONG type, PSECURITY_DESCRIPTOR security = nullptr) {
    if (name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
        return std::unexpected(Error{ErrorCode::invalid_path});
    auto text = wide(name);
    if (!text)
        return std::unexpected(text.error());
    UNICODE_STRING unicode{};
    unicode.Length = static_cast<USHORT>(text->size() * sizeof(wchar_t));
    unicode.MaximumLength = unicode.Length;
    unicode.Buffer = text->data();
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.RootDirectory = parent;
    attributes.ObjectName = &unicode;
    attributes.Attributes = OBJ_CASE_INSENSITIVE;
    attributes.SecurityDescriptor = security;
    IO_STATUS_BLOCK io{};
    HANDLE value = nullptr;
    const auto status =
        NtCreateFile(&value, access | SYNCHRONIZE | FILE_READ_ATTRIBUTES, &attributes, &io, nullptr,
                     FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     disposition, open_reparse_point | synchronous_nonalert | type, nullptr, 0);
    if (status < 0)
        return std::unexpected(nt_error(status));
    return File(value);
}
Result<File> checked(File file, Kind kind) {
    auto info = metadata(file.get());
    if (!info)
        return std::unexpected(info.error());
    if (info->kind == Kind::other || (kind != Kind::other && info->kind != kind))
        return std::unexpected(Error{ErrorCode::invalid_path});
    return file;
}
Result<void> position(Handle file, std::uint64_t offset) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
        return std::unexpected(Error{ErrorCode::invalid_manifest});
    LARGE_INTEGER value{};
    value.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, value, nullptr, FILE_BEGIN))
        return std::unexpected(system_error());
    return {};
}
// Flexible SDK structures are backed by suitably aligned, bounded storage.
struct NameInformation {
    BOOLEAN replace;
    HANDLE root;
    ULONG length;
    WCHAR name[1];
};
Result<void> set_name(Handle file, Handle root, const std::string& name,
                      FILE_INFORMATION_CLASS type) {
    auto text = wide(name);
    if (!text)
        return std::unexpected(text.error());
    const auto bytes = offsetof(NameInformation, name) + text->size() * sizeof(wchar_t);
    std::vector<std::uint64_t> storage((bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
    auto* info = reinterpret_cast<NameInformation*>(storage.data());
    info->replace = FALSE;
    info->root = root;
    info->length = static_cast<ULONG>(text->size() * sizeof(wchar_t));
    std::memcpy(info->name, text->data(), info->length);
    IO_STATUS_BLOCK io{};
    const auto status = NtSetInformationFile(file, &io, info, static_cast<ULONG>(bytes), type);
    if (status < 0)
        return std::unexpected(nt_error(status));
    return {};
}
} // namespace
File::~File() {
    if (handle_)
        CloseHandle(handle_);
}
File::File(File&& other) noexcept
    : lock_file(std::move(other.lock_file)), handle_(std::exchange(other.handle_, invalid)) {}
File& File::operator=(File&& other) noexcept {
    File temporary(std::move(other));
    std::swap(handle_, temporary.handle_);
    lock_file.swap(temporary.lock_file);
    return *this;
}
Result<void> receive_root(Handle root) {
    std::array<wchar_t, 32768> path{};
    const auto size = GetFinalPathNameByHandleW(root, path.data(), static_cast<DWORD>(path.size()),
                                                FILE_NAME_NORMALIZED);
    if (!size || size >= path.size())
        return std::unexpected(system_error());
    if (std::wstring_view(path.data(), size).starts_with(L"\\\\?\\UNC\\"))
        return std::unexpected(Error{ErrorCode::unsupported_platform});
    std::array<wchar_t, 32> name{};
    DWORD flags = 0;
    if (!GetVolumeInformationByHandleW(root, nullptr, 0, nullptr, nullptr, &flags, name.data(),
                                       static_cast<DWORD>(name.size())))
        return std::unexpected(system_error());
    if (std::wstring_view(name.data()) != L"NTFS" || !(flags & FILE_PERSISTENT_ACLS) ||
        !(flags & FILE_SUPPORTS_HARD_LINKS))
        return std::unexpected(Error{ErrorCode::unsupported_platform});
    return {};
}
Result<File> open_path(const std::filesystem::path& selected, Kind kind) {
    const auto path = normalized(selected);
    const auto& name = path.native();
    if (name.empty() || name.size() > 32760 || name.find(L'\0') != std::wstring::npos ||
        name.starts_with(L"\\\\.\\") || name.starts_with(L"\\\\?\\GLOBALROOT"))
        return std::unexpected(Error{ErrorCode::invalid_path});
    const DWORD required = GetFullPathNameW(name.c_str(), 0, nullptr, nullptr);
    if (!required || required > 32760)
        return std::unexpected(Error{ErrorCode::invalid_path});
    std::wstring absolute(required, L'\0');
    const DWORD length = GetFullPathNameW(name.c_str(), required, absolute.data(), nullptr);
    if (!length || length >= required)
        return std::unexpected(system_error());
    absolute.resize(length);
    if (!absolute.starts_with(L"\\\\?\\")) {
        if (absolute.starts_with(L"\\\\"))
            absolute = L"\\\\?\\UNC\\" + absolute.substr(2);
        else
            absolute = L"\\\\?\\" + absolute;
    }
    const bool drive = absolute.size() >= 7 && absolute.starts_with(L"\\\\?\\") &&
                       absolute[5] == L':' && absolute[6] == L'\\';
    if (!drive && !absolute.starts_with(L"\\\\?\\UNC\\"))
        return std::unexpected(Error{ErrorCode::invalid_path});
    const auto value = CreateFileW(
        absolute.c_str(), FILE_GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (value == INVALID_HANDLE_VALUE)
        return std::unexpected(system_error());
    return checked(File(value), kind);
}
Result<File> open_at(Handle parent, const std::string& name, Access mode, Kind kind) {
    const bool creating = mode == Access::create_file || mode == Access::create_directory;
    if (mode == Access::create_directory)
        kind = Kind::directory;
    PrivateSecurity security;
    if (creating) {
        auto ready = security.initialize();
        if (!ready)
            return std::unexpected(ready.error());
    }
    ACCESS_MASK access = FILE_GENERIC_READ;
    if (mode == Access::update || mode == Access::create_file)
        access |= FILE_GENERIC_WRITE | DELETE;
    if (mode == Access::create_directory)
        access |= DELETE;
    auto file = relative(parent, name, creating ? disposition_create : disposition_open, access,
                         kind == Kind::directory ? directory_file : non_directory_file,
                         creating ? &security.descriptor : nullptr);
    if (!file)
        return std::unexpected(file.error());
    return checked(std::move(*file), kind);
}
Result<File> duplicate(Handle value) {
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), value, GetCurrentProcess(), &copy, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
        return std::unexpected(system_error());
    return File(copy);
}
Result<Metadata> metadata(Handle file) {
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandle(file, &info) ||
        !GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)))
        return std::unexpected(system_error());
    Metadata result;
    if (GetFileType(file) == FILE_TYPE_DISK &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        result.kind =
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? Kind::directory : Kind::regular;
    result.device = info.dwVolumeSerialNumber;
    result.identity = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) | info.nFileIndexLow;
    result.size = result.kind == Kind::directory
                      ? 0
                      : (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
    result.links = info.nNumberOfLinks;
    result.times = {basic.LastWriteTime.QuadPart, 0, basic.ChangeTime.QuadPart, 0};
    return result;
}
Result<std::optional<Metadata>> child_metadata(Handle parent, const std::string& name) {
    auto file = relative(parent, name, disposition_open, FILE_READ_ATTRIBUTES, 0);
    if (!file) {
        if (missing(file.error()))
            return std::optional<Metadata>{};
        return std::unexpected(file.error());
    }
    auto info = metadata(file->get());
    if (!info)
        return std::unexpected(info.error());
    return std::optional(*info);
}
Result<void> validate_private(Handle file) {
    PrivateSecurity current;
    auto ready = current.initialize();
    if (!ready)
        return ready;
    const auto sid = reinterpret_cast<const TOKEN_USER*>(current.token.data())->User.Sid;
    DWORD size = 0;
    if (GetKernelObjectSecurity(file, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                nullptr, 0, &size) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || size > 65536)
        return std::unexpected(Error{ErrorCode::invalid_checkpoint});
    std::vector<std::uint64_t> storage((size + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
    auto* descriptor = reinterpret_cast<SECURITY_DESCRIPTOR*>(storage.data());
    if (!GetKernelObjectSecurity(file, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                 descriptor, size, &size))
        return std::unexpected(system_error());
    PSID owner = nullptr;
    BOOL defaulted = FALSE, present = FALSE;
    PACL acl = nullptr;
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    if (!GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) || !EqualSid(owner, sid) ||
        !GetSecurityDescriptorControl(descriptor, &control, &revision) ||
        !(control & SE_DACL_PROTECTED) ||
        !GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) || !present || !acl ||
        acl->AceCount != 1)
        return std::unexpected(Error{ErrorCode::invalid_checkpoint});
    void* raw = nullptr;
    if (!GetAce(acl, 0, &raw))
        return std::unexpected(system_error());
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags != 0 ||
        !EqualSid(const_cast<DWORD*>(&ace->SidStart), sid) || ace->Mask != FILE_ALL_ACCESS)
        return std::unexpected(Error{ErrorCode::invalid_checkpoint});
    return {};
}
Result<void> lock(File& file) {
    auto info = metadata(file.get());
    if (!info)
        return std::unexpected(info.error());
    auto private_object = validate_private(file.get());
    if (!private_object)
        return private_object;
    Handle target = file.get();
    if (info->kind == Kind::directory) {
        auto guard = open_at(target, ".bridge-lock", Access::create_file);
        if (!guard && guard.error().code == ErrorCode::destination_conflict)
            guard = open_at(target, ".bridge-lock", Access::update);
        if (!guard)
            return std::unexpected(guard.error());
        auto private_file = validate_private(guard->get());
        if (!private_file)
            return private_file;
        auto lock_info = metadata(guard->get());
        if (!lock_info)
            return std::unexpected(lock_info.error());
        if (lock_info->links != 1)
            return std::unexpected(Error{ErrorCode::invalid_checkpoint});
        file.lock_file = std::make_unique<File>(std::move(*guard));
        target = file.lock_file->get();
    }
    OVERLAPPED region{};
    // The lock lies beyond all bounded journal data; Windows byte locks block IO
    // from other handles, unlike POSIX flock. Keep parsing IO outside this range.
    region.Offset = 0xffffffffU;
    region.OffsetHigh = 0x7fffffffU;
    if (!LockFileEx(target, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &region))
        return std::unexpected(system_error());
    return {};
}
Result<void> read_at(Handle file, std::span<std::uint8_t> output, std::uint64_t offset,
                     ErrorCode end_error, std::stop_token stop) {
    auto moved = position(file, offset);
    if (!moved)
        return moved;
    while (!output.empty()) {
        if (stop.stop_requested())
            return std::unexpected(Error{ErrorCode::cancelled});
        const auto amount = static_cast<DWORD>(std::min<std::size_t>(output.size(), 1048576));
        DWORD count = 0;
        if (!ReadFile(file, output.data(), amount, &count, nullptr))
            return std::unexpected(system_error());
        if (count == 0)
            return std::unexpected(Error{end_error});
        output = output.subspan(count);
    }
    return {};
}
Result<void> write_at(Handle file, std::span<const std::uint8_t> input, std::uint64_t offset) {
    auto moved = position(file, offset);
    if (!moved)
        return moved;
    while (!input.empty()) {
        const auto amount = static_cast<DWORD>(std::min<std::size_t>(input.size(), 1048576));
        DWORD count = 0;
        if (!WriteFile(file, input.data(), amount, &count, nullptr))
            return std::unexpected(system_error());
        if (count == 0)
            return std::unexpected(Error{ErrorCode::io_failed});
        input = input.subspan(count);
    }
    return {};
}
Result<void> truncate(Handle file, std::uint64_t size) {
    auto moved = position(file, size);
    if (!moved)
        return moved;
    if (!SetEndOfFile(file))
        return std::unexpected(system_error());
    return {};
}
Result<void> sync(Handle file) {
    auto info = metadata(file);
    if (!info)
        return std::unexpected(info.error());
    // Windows has no portable directory-fsync equivalent. Namespace changes are
    // atomic, while data/journal durability uses FlushFileBuffers. Recovery must
    // always validate state; power-loss guarantees remain filesystem dependent.
    if (info->kind == Kind::directory)
        return {};
    if (!FlushFileBuffers(file))
        return std::unexpected(system_error());
    return {};
}
Result<void> remove(Handle parent, const std::string& name) {
    auto file = relative(parent, name, disposition_open, DELETE, 0);
    if (!file) {
        if (missing(file.error()))
            return {};
        return std::unexpected(file.error());
    }
    FILE_DISPOSITION_INFO_EX info{FILE_DISPOSITION_FLAG_DELETE |
                                  FILE_DISPOSITION_FLAG_POSIX_SEMANTICS};
    if (!SetFileInformationByHandle(file->get(), FileDispositionInfoEx, &info, sizeof(info)))
        return std::unexpected(system_error());
    return {};
}
Result<void> link(Handle file, Handle root, const std::string&, const std::string& to) {
    return set_name(file, root, to, static_cast<FILE_INFORMATION_CLASS>(11)); // FileLinkInformation
}
Result<void> rename_directory(Handle stage, Handle, Handle root, const std::string& name) {
    return set_name(stage, root, name,
                    static_cast<FILE_INFORMATION_CLASS>(10)); // FileRenameInformation
}
Result<std::vector<Entry>> list(Handle file, std::size_t& count, std::size_t maximum,
                                std::stop_token stop) {
    std::vector<std::uint64_t> storage(8192); // 64 KiB, suitably aligned.
    std::vector<Entry> entries;
    bool first = true;
    while (true) {
        if (stop.stop_requested())
            return std::unexpected(Error{ErrorCode::cancelled});
        if (!GetFileInformationByHandleEx(
                file, first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
                storage.data(), static_cast<DWORD>(storage.size() * sizeof(std::uint64_t)))) {
            if (GetLastError() == ERROR_NO_MORE_FILES)
                break;
            return std::unexpected(system_error());
        }
        first = false;
        std::size_t offset = 0;
        while (true) {
            const auto* item = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
                reinterpret_cast<const std::uint8_t*>(storage.data()) + offset);
            const auto available = storage.size() * sizeof(std::uint64_t) - offset;
            if (available < offsetof(FILE_ID_BOTH_DIR_INFO, FileName) ||
                item->FileNameLength % sizeof(wchar_t) != 0 ||
                item->FileNameLength > available - offsetof(FILE_ID_BOTH_DIR_INFO, FileName))
                return std::unexpected(Error{ErrorCode::io_failed});
            auto name = utf8({item->FileName, item->FileNameLength / sizeof(wchar_t)});
            if (!name)
                return std::unexpected(name.error());
            if (*name != "." && *name != "..") {
                if (++count > maximum)
                    return std::unexpected(Error{ErrorCode::invalid_manifest});
                auto info = child_metadata(file, *name);
                if (!info)
                    return std::unexpected(info.error());
                if (!*info)
                    return std::unexpected(Error{ErrorCode::source_changed});
                entries.push_back({std::move(*name), **info});
            }
            if (!item->NextEntryOffset)
                break;
            if (item->NextEntryOffset >= available ||
                item->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO) != 0)
                return std::unexpected(Error{ErrorCode::io_failed});
            offset += item->NextEntryOffset;
        }
    }
    std::ranges::sort(entries, {}, &Entry::name);
    return entries;
}
Result<std::filesystem::path> temporary_directory() {
    std::error_code ec;
    auto temporary = std::filesystem::temp_directory_path(ec);
    if (ec)
        return std::unexpected(Error{ErrorCode::io_failed, ec.value()});
    auto root = open_root(temporary);
    if (!root)
        return std::unexpected(root.error());
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        LUID id{};
        if (!AllocateLocallyUniqueId(&id))
            return std::unexpected(system_error());
        // Exclusive creation supplies uniqueness/security; no identity is sent.
        const auto name =
            "bridge-folder-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(id.HighPart)) << 32U) |
                id.LowPart);
        auto created = open_at(root->get(), name, Access::create_directory);
        if (created)
            return temporary / name;
        if (created.error().code != ErrorCode::destination_conflict)
            return std::unexpected(created.error());
    }
    return std::unexpected(Error{ErrorCode::destination_conflict});
}
} // namespace bridge::io::native
