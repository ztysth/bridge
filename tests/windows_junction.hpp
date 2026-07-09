#pragma once
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstring>
#include <vector>
#include <windows.h>
#include <winioctl.h>
namespace bridge::test {
// A mount-point reparse fixture needs no symbolic-link privilege/Developer Mode.
inline void directory_link(const std::filesystem::path& target, const std::filesystem::path& link) {
    std::filesystem::create_directory(link);
    const auto value =
        CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    REQUIRE(value != INVALID_HANDLE_VALUE);
    struct Owner {
        HANDLE value;
        ~Owner() { CloseHandle(value); }
    } owner{value};
    const auto substitute = L"\\??\\" + std::filesystem::absolute(target).native();
    const auto print = std::filesystem::absolute(target).native();
    struct Header {
        DWORD tag;
        WORD data_length, reserved;
        WORD substitute_offset, substitute_length, print_offset, print_length;
    };
    const auto path_bytes = (substitute.size() + print.size() + 2) * sizeof(wchar_t);
    std::vector<DWORD> storage((sizeof(Header) + path_bytes + sizeof(DWORD) - 1) / sizeof(DWORD));
    auto* info = reinterpret_cast<Header*>(storage.data());
    info->tag = IO_REPARSE_TAG_MOUNT_POINT;
    info->data_length = static_cast<WORD>(8 + path_bytes);
    info->substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    info->print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    info->print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
    auto* paths = reinterpret_cast<std::uint8_t*>(storage.data()) + sizeof(Header);
    std::memcpy(paths, substitute.data(), info->substitute_length);
    std::memcpy(paths + info->print_offset, print.data(), info->print_length);
    DWORD returned = 0;
    REQUIRE(DeviceIoControl(owner.value, FSCTL_SET_REPARSE_POINT, storage.data(),
                            static_cast<DWORD>(sizeof(Header) + path_bytes), nullptr, 0, &returned,
                            nullptr));
}
} // namespace bridge::test
#else
namespace bridge::test {
inline void directory_link(const std::filesystem::path& target, const std::filesystem::path& link) {
    std::filesystem::create_directory_symlink(target, link);
}
} // namespace bridge::test
#endif
