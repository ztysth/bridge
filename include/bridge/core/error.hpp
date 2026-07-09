#pragma once
#include <cstdint>
#include <expected>
#include <string_view>
namespace bridge {
enum class ErrorCode : std::uint16_t {
    malformed_frame = 1,
    oversized_frame,
    unsupported_version,
    invalid_state,
    rejected,
    cancelled,
    disconnected,
    timeout,
    invalid_address,
    listen_failed,
    connect_failed,
    tls_failed,
    identity_failed,
    queue_full,
    invalid_path,
    peer_error,
    disk_full,
    permission_denied,
    io_failed,
    destination_conflict,
    checksum_mismatch,
    invalid_manifest,
    invalid_checkpoint,
    checkpoint_busy,
    unsupported_platform,
    hash_failed,
    injected_failure,
    source_changed
};
struct Error {
    ErrorCode code;
    // Numeric OS context only; never paths or third-party diagnostic strings.
    std::int32_t native_code = 0;
};
template <typename T> using Result = std::expected<T, Error>;
constexpr std::string_view error_message(ErrorCode code) {
    switch (code) {
    case ErrorCode::malformed_frame:
        return "Peer sent an invalid message. Start a new session.";
    case ErrorCode::oversized_frame:
        return "Peer exceeded message limits. Connection closed.";
    case ErrorCode::unsupported_version:
        return "Peer protocol is incompatible. Update both applications.";
    case ErrorCode::invalid_state:
        return "Peer sent a message out of order. Start a new session.";
    case ErrorCode::rejected:
        return "Pairing rejected. Check the intended peer and start again.";
    case ErrorCode::cancelled:
        return "Session cancelled.";
    case ErrorCode::disconnected:
        return "Connection interrupted before completion. Start again.";
    case ErrorCode::timeout:
        return "Session timed out. Check the peer and start again.";
    case ErrorCode::invalid_address:
        return "Select a local private IPv4 address and a valid port.";
    case ErrorCode::listen_failed:
        return "Cannot receive on this address. Check the interface and port.";
    case ErrorCode::connect_failed:
        return "Cannot connect. Check receive mode, address and firewall.";
    case ErrorCode::tls_failed:
        return "Secure connection failed. TLS 1.3 and valid identities are required.";
    case ErrorCode::identity_failed:
        return "Cannot create or validate an ephemeral identity.";
    case ErrorCode::queue_full:
        return "Peer exceeded the session buffer limit. Connection closed.";
    case ErrorCode::invalid_path:
        return "Path is not portable or safe. Rename the affected entry.";
    case ErrorCode::peer_error:
        return "Peer ended the session with an error.";
    case ErrorCode::disk_full:
        return "Storage is full. Free space for temporary data and retry from the checkpoint.";
    case ErrorCode::permission_denied:
        return "Cannot access storage. Check source-file and destination-folder permissions.";
    case ErrorCode::io_failed:
        return "Storage operation failed. Check the filesystem and reopen the checkpoint.";
    case ErrorCode::destination_conflict:
        return "Destination already exists. Choose another name or folder.";
    case ErrorCode::checksum_mismatch:
        return "File integrity verification failed. Keep the partial and restart safely.";
    case ErrorCode::invalid_manifest:
        return "Item description is invalid or exceeds supported limits.";
    case ErrorCode::invalid_checkpoint:
        return "Checkpoint is damaged or does not match this file. Do not trust the partial.";
    case ErrorCode::checkpoint_busy:
        return "Another receiver is using this checkpoint. Close it before resuming.";
    case ErrorCode::unsupported_platform:
        return "Safe file IO is unavailable for this platform or filesystem. On Windows, choose a "
               "local NTFS destination.";
    case ErrorCode::hash_failed:
        return "Content hashing failed. Check the cryptographic backend.";
    case ErrorCode::source_changed:
        return "Source changed. Start a new transfer with a fresh snapshot.";
    case ErrorCode::injected_failure:
        return "A diagnostic checkpoint failure was injected.";
    }
    return "Unknown session error.";
}
} // namespace bridge
