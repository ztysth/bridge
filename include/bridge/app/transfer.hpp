#pragma once
#include "bridge/app/session.hpp"
#include "bridge/core/file_protocol.hpp"
#include "bridge/io/checkpoint.hpp"
#include <filesystem>
#include <memory>
#include <optional>
namespace bridge::app {
class FileWorker;
enum class TransferPhase {
    idle,
    preparing,
    pairing,
    awaiting_offer,
    awaiting_accept,
    transferring,
    pausing,
    paused,
    verifying,
    closing,
    complete,
    failed
};
enum class ReceiveMode { create, resume };
class Transfer final : public QObject {
    Q_OBJECT
  public:
    Transfer(Role role, Logger& logger);
    ~Transfer() override;
    static bool supported();
    Result<std::uint16_t> receive(const QHostAddress& address, std::uint16_t port,
                                  std::filesystem::path root,
                                  ReceiveMode mode = ReceiveMode::create);
    Result<void> send_file(const QHostAddress& address, std::uint16_t port,
                           std::filesystem::path source, std::optional<FileManifest> resume = {});
    void confirm() { session_.confirm(); }
    void reject() { session_.reject(); }
    void cancel();
    Result<void> accept();
    Result<void> pause();
    Result<void> continue_transfer();
    bool can_confirm() const { return session_.can_confirm(); }
    bool authenticated() const { return session_.paired() && (!terminal_ || success_); }
    bool can_accept() const {
        return role_ == Role::receiver && phase_ == TransferPhase::awaiting_accept;
    }
    bool can_pause() const { return flow_phase() && !pause_.local_held() && !pause_.paused(); }
    bool can_continue() const { return flow_phase() && pause_.paused() && pause_.local_held(); }
    bool local_paused() const { return pause_.local_held(); }
    bool peer_paused() const { return pause_.remote_held(); }
    bool busy() const { return phase_ != TransferPhase::idle && !finished_; }
    bool listening() const { return session_.listening(); }
    TransferPhase phase() const { return phase_; }
    std::uint64_t durable_bytes() const { return durable_; }
    const std::optional<FileManifest>& manifest() const { return manifest_; }
    QString status() const;
    // Test-only deterministic failures reuse production IO/transport.
    Session& diagnostic_session() { return session_; }
    void diagnostic_injection(io::CheckpointInjection injection) { injection_ = injection; }
  signals:
    void pairing_pending(QString fingerprint);
    void changed();
    void finished(bool success, int error_code);

  private:
    bool flow_phase() const;
    void ready();
    void offer_if_ready();
    void handle(const Frame& frame);
    void receive_chunk(const Frame& frame);
    void receive_prefix(const Frame& frame);
    void receive_barrier(const Frame& frame);
    void verify();
    bool send(Message type, std::span<const std::uint8_t> payload = {});
    void schedule();
    void pump();
    void flow_changed();
    void fail(Error error);
    void terminal(bool success, ErrorCode code);
    void worker_stopped();
    void notify();
    Role role_;
    Logger& logger_;
    std::uint64_t diagnostic_id_;
    TransferPhase reported_phase_ = TransferPhase::idle;
    std::uint64_t reported_prefix_ = 0;
    Session session_;
    std::unique_ptr<FileWorker> worker_;
    QTimer heartbeat_;
    PauseState pause_;
    TransferPhase phase_ = TransferPhase::idle;
    std::optional<FileManifest> manifest_;
    std::filesystem::path root_;
    ReceiveMode mode_ = ReceiveMode::create;
    io::CheckpointInjection injection_;
    std::optional<Barrier> pending_barrier_;
    // At most one speculative chunk, independent of the one outstanding wire chunk.
    std::optional<Chunk> lookahead_;
    std::uint64_t durable_ = 0, expected_ack_ = 0;
    bool checkpoint_opening_ = false;
    bool session_ready_ = false, io_busy_ = false, in_flight_ = false, pump_pending_ = false;
    bool terminal_ = false, finished_ = false, success_ = false;
    ErrorCode error_ = ErrorCode::cancelled;
};
} // namespace bridge::app
