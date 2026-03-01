#include "bridge/app/transfer.hpp"
#include "bridge/io/path.hpp"
#include "file_worker.hpp"
#include <QRandomGenerator>
#include <algorithm>
namespace bridge::app {
namespace {
TransferId random_id() {
    TransferId id{};
    for (auto& byte : id)
        byte = static_cast<std::uint8_t>(QRandomGenerator::system()->generate());
    return id;
}
bool matches_id(const Frame& frame, const FileManifest& m) {
    return frame.payload.size() == m.id.size() && std::ranges::equal(frame.payload, m.id);
}
} // namespace
Transfer::Transfer(Role role, Logger& logger)
    : role_(role), logger_(logger), diagnostic_id_(QRandomGenerator::system()->generate64()),
      session_(role, logger, nullptr, SessionPurpose::file),
      worker_(std::make_unique<FileWorker>()), pause_(role) {
    QObject::connect(&session_, &Session::pairing_pending, this, [this](const QString& f) {
        if (terminal_)
            return;
        phase_ = TransferPhase::pairing;
        emit pairing_pending(f);
        notify();
    });
    QObject::connect(&session_, &Session::changed, this, &Transfer::notify);
    QObject::connect(&session_, &Session::ready, this, &Transfer::ready);
    QObject::connect(&session_, &Session::application_frame, this, &Transfer::handle);
    QObject::connect(&session_, &Session::finished, this, [this](bool ok, int code) {
        if (terminal_)
            return;
        if (ok && phase_ != TransferPhase::closing) {
            fail(Error{ErrorCode::invalid_state});
            return;
        }
        terminal(ok, static_cast<ErrorCode>(code));
    });
    QObject::connect(worker_.get(), &FileWorker::stopped, this, &Transfer::worker_stopped);
    heartbeat_.setInterval(2000);
    QObject::connect(&heartbeat_, &QTimer::timeout, this, [this] { send(Message::heartbeat); });
}
Transfer::~Transfer() {
    heartbeat_.stop();
    worker_->stop();
}
bool Transfer::supported() { return io::supports_file_io(); }
bool Transfer::flow_phase() const {
    return phase_ == TransferPhase::transferring || phase_ == TransferPhase::pausing ||
           phase_ == TransferPhase::paused;
}
QString Transfer::status() const {
    switch (phase_) {
    case TransferPhase::idle:
        return QStringLiteral("Choose a file or folder to send, or a destination to receive.");
    case TransferPhase::preparing:
        return QStringLiteral("Preparing and verifying your selection. You can cancel.");
    case TransferPhase::pairing:
        return QStringLiteral("Compare every fingerprint digit on both screens, then confirm.");
    case TransferPhase::awaiting_offer:
        return QStringLiteral("Receive mode enabled. Waiting for the authenticated sender.");
    case TransferPhase::awaiting_accept:
        return role_ == Role::receiver
                   ? QStringLiteral(
                         "Review the offered item and selected destination, then Accept.")
                   : QStringLiteral("Waiting for the receiver to accept this item.");
    case TransferPhase::transferring:
        return QStringLiteral("Transferring. Progress counts durably checkpointed bytes.");
    case TransferPhase::pausing:
        return QStringLiteral("Pausing: saving the in-flight chunk and confirming the checkpoint.");
    case TransferPhase::paused:
        return pause_.local_held()
                   ? QStringLiteral(
                         "Paused. Continue releases your pause; a peer pause may remain.")
                   : QStringLiteral("Paused by peer. Waiting for their Continue.");
    case TransferPhase::verifying:
        if (checkpoint_opening_)
            return QStringLiteral("Validating destination checkpoint. You can cancel.");
        return QStringLiteral(
            "Verifying integrity and preparing the received item. You can cancel.");
    case TransferPhase::closing:
        return QStringLiteral("Item verified and committed. Closing the secure connection.");
    case TransferPhase::complete:
        return QStringLiteral("Transfer complete. Integrity verified.");
    case TransferPhase::failed: {
        auto message = QString::fromUtf8(error_message(error_).data());
        if (manifest_ && durable_ > 0 &&
            (error_ == ErrorCode::cancelled || error_ == ErrorCode::disconnected ||
             error_ == ErrorCode::connect_failed || error_ == ErrorCode::timeout))
            message += QStringLiteral(" Checkpoint preserved. Keep the sender window open, enable "
                                      "Resume on both sides and pair again.");
        return message;
    }
    }
    return {};
}
Result<std::uint16_t> Transfer::receive(const QHostAddress& address, std::uint16_t port,
                                        std::filesystem::path root, ReceiveMode mode) {
    if (!supported())
        return std::unexpected(Error{ErrorCode::unsupported_platform});
    if (role_ != Role::receiver || phase_ != TransferPhase::idle || root.empty())
        return std::unexpected(Error{ErrorCode::invalid_state});
    root_ = std::move(root);
    mode_ = mode;
    auto result = session_.receive(address, port);
    if (!result)
        return result;
    phase_ = TransferPhase::awaiting_offer;
    notify();
    return result;
}
Result<void> Transfer::send_file(const QHostAddress& address, std::uint16_t port,
                                 std::filesystem::path source, std::optional<FileManifest> resume) {
    if (!supported())
        return std::unexpected(Error{ErrorCode::unsupported_platform});
    if (role_ != Role::initiator || phase_ != TransferPhase::idle)
        return std::unexpected(Error{ErrorCode::invalid_state});
    auto connected = session_.connect_peer(address, port);
    if (!connected)
        return connected;
    phase_ = TransferPhase::preparing;
    io_busy_ = true;
    auto submitted = worker_->submit(
        [path = std::move(source), expected = std::move(resume)](
            FileWorker::State& state, std::stop_token stop) -> Result<FileWorker::Value> {
            auto file = io::SourcePayload::open(path, expected ? expected->id : random_id(), stop);
            if (!file)
                return std::unexpected(file.error());
            if (expected && file->manifest() != *expected)
                return std::unexpected(Error{ErrorCode::source_changed});
            auto manifest = file->manifest();
            state.source = std::move(*file);
            return manifest;
        },
        [this](Result<FileWorker::Value> result) {
            io_busy_ = false;
            if (terminal_)
                return;
            if (!result) {
                fail(result.error());
                return;
            }
            manifest_ = std::get<FileManifest>(std::move(*result));
            offer_if_ready();
            notify();
        });
    if (!submitted) {
        fail(submitted.error());
        return submitted;
    }
    notify();
    return {};
}
void Transfer::ready() {
    session_ready_ = true;
    heartbeat_.start();
    if (role_ == Role::initiator) {
        if (!manifest_) {
            phase_ = TransferPhase::preparing;
            notify();
        }
        offer_if_ready();
    } else {
        phase_ = TransferPhase::awaiting_offer;
        notify();
    }
}
void Transfer::offer_if_ready() {
    if (!session_ready_ || !manifest_ || terminal_ || role_ != Role::initiator)
        return;
    phase_ = TransferPhase::awaiting_accept;
    send(Message::offer, encode_offer(*manifest_));
}
bool Transfer::send(Message type, std::span<const std::uint8_t> payload) {
    if (terminal_)
        return false;
    auto result = session_.send_application(type, payload);
    if (!result) {
        fail(result.error());
        return false;
    }
    return true;
}
Result<void> Transfer::accept() {
    if (!can_accept())
        return std::unexpected(Error{ErrorCode::invalid_state});
    io_busy_ = true;
    checkpoint_opening_ = true;
    phase_ = TransferPhase::verifying;
    notify();
    auto result = worker_->submit(
        [root = root_, m = *manifest_, mode = mode_, injection = injection_](
            FileWorker::State& state, std::stop_token stop) -> Result<FileWorker::Value> {
            if (stop.stop_requested())
                return std::unexpected(Error{ErrorCode::cancelled});
            auto payload_root = root;
            if (m.kind == PayloadKind::folder) {
                auto folder = io::FolderDestination::open(root, m, mode == ReceiveMode::resume);
                if (!folder)
                    return std::unexpected(folder.error());
                payload_root = folder->payload_root();
                state.folder = std::move(*folder);
            }
            auto partial = mode == ReceiveMode::resume
                               ? io::PartialFile::resume(payload_root, m, injection, stop)
                               : io::PartialFile::create(payload_root, m, injection);
            if (!partial)
                return std::unexpected(partial.error());
            auto prefix = partial->durable_bytes();
            state.partial = std::move(*partial);
            return prefix;
        },
        [this](Result<FileWorker::Value> value) {
            io_busy_ = false;
            checkpoint_opening_ = false;
            if (terminal_)
                return;
            if (!value) {
                fail(value.error());
                return;
            }
            durable_ = std::get<std::uint64_t>(*value);
            phase_ = TransferPhase::transferring;
            if (send(Message::accept_prefix, encode_prefix({manifest_->id, durable_})))
                flow_changed();
        });
    if (!result)
        fail(result.error());
    return result;
}
Result<void> Transfer::pause() {
    if (!can_pause())
        return std::unexpected(Error{ErrorCode::invalid_state});
    auto hold = pause_.pause();
    if (!hold)
        return std::unexpected(hold.error());
    pending_barrier_.reset();
    if (!send(Message::hold, encode_hold(*hold)))
        return std::unexpected(Error{error_});
    flow_changed();
    return {};
}
Result<void> Transfer::continue_transfer() {
    if (!can_continue())
        return std::unexpected(Error{ErrorCode::invalid_state});
    auto hold = pause_.continue_transfer();
    if (!hold)
        return std::unexpected(hold.error());
    pending_barrier_.reset();
    if (!send(Message::hold, encode_hold(*hold)))
        return std::unexpected(Error{error_});
    flow_changed();
    return {};
}
void Transfer::flow_changed() {
    if (terminal_ || !flow_phase())
        return;
    phase_ = pause_.paused() ? TransferPhase::paused
             : pause_.held() ? TransferPhase::pausing
                             : TransferPhase::transferring;
    notify();
    schedule();
}
void Transfer::schedule() {
    if (pump_pending_ || terminal_)
        return;
    pump_pending_ = true;
    QTimer::singleShot(0, this, [this] {
        pump_pending_ = false;
        pump();
    });
}
void Transfer::pump() {
    if (terminal_ || role_ != Role::initiator || !flow_phase() || io_busy_ || in_flight_)
        return;
    if (pause_.held()) {
        if (pause_.paused())
            return;
        auto barrier = pause_.barrier(durable_);
        if (!pending_barrier_ || *pending_barrier_ != barrier) {
            pending_barrier_ = barrier;
            send(Message::pause_barrier, encode_barrier(barrier));
        }
        return;
    }
    if (durable_ == manifest_->size) {
        phase_ = TransferPhase::verifying;
        send(Message::finish_file, manifest_->id);
        notify();
        return;
    }
    io_busy_ = true;
    auto submitted = worker_->submit(
        [offset = durable_](FileWorker::State& state,
                            std::stop_token stop) -> Result<FileWorker::Value> {
            auto chunk = state.source->read(offset, stop);
            if (!chunk)
                return std::unexpected(chunk.error());
            return std::move(*chunk);
        },
        [this](Result<FileWorker::Value> result) {
            io_busy_ = false;
            if (terminal_)
                return;
            if (!result) {
                fail(result.error());
                return;
            }
            if (pause_.held()) {
                schedule();
                return;
            } // Discard an unsent read; no checkpoint moved.
            const auto& chunk = std::get<Chunk>(*result);
            expected_ack_ = chunk.offset + chunk.data.size();
            in_flight_ = true;
            send(Message::data_chunk, encode_chunk(chunk));
        });
    if (!submitted)
        fail(submitted.error());
}
void Transfer::receive_prefix(const Frame& frame) {
    auto p = decode_prefix(frame.payload);
    if (!p || !manifest_ || p->id != manifest_->id || !resume_index(manifest_->size, p->bytes)) {
        fail(Error{ErrorCode::invalid_checkpoint});
        return;
    }
    if (frame.type == Message::accept_prefix) {
        if (role_ != Role::initiator || phase_ != TransferPhase::awaiting_accept) {
            fail(Error{ErrorCode::invalid_state});
            return;
        }
        durable_ = p->bytes;
        phase_ = TransferPhase::transferring;
        flow_changed();
        return;
    }
    if (role_ != Role::initiator || !flow_phase() || !in_flight_ || p->bytes != expected_ack_) {
        fail(Error{ErrorCode::invalid_state});
        return;
    }
    durable_ = p->bytes;
    in_flight_ = false;
    flow_changed();
}
void Transfer::receive_chunk(const Frame& frame) {
    if (role_ != Role::receiver || !flow_phase() || pause_.paused() || io_busy_) {
        fail(Error{ErrorCode::invalid_state});
        return;
    }
    auto chunk = decode_chunk(frame.payload, *manifest_);
    if (!chunk) {
        fail(chunk.error());
        return;
    }
    if (chunk->offset != durable_) {
        fail(Error{ErrorCode::invalid_state});
        return;
    }
    io_busy_ = true;
    auto submitted = worker_->submit(
        [c = std::move(*chunk)](FileWorker::State& state,
                                std::stop_token stop) -> Result<FileWorker::Value> {
            auto prefix = state.partial->append(c.data, c.digest, stop);
            if (!prefix)
                return std::unexpected(prefix.error());
            return *prefix;
        },
        [this](Result<FileWorker::Value> result) {
            io_busy_ = false;
            if (terminal_)
                return;
            if (!result) {
                fail(result.error());
                return;
            }
            durable_ = std::get<std::uint64_t>(*result);
            if (send(Message::durable_ack, encode_prefix({manifest_->id, durable_})))
                flow_changed();
        });
    if (!submitted)
        fail(submitted.error());
}
void Transfer::receive_barrier(const Frame& frame) {
    if (!flow_phase()) {
        fail(Error{ErrorCode::invalid_state});
        return;
    }
    auto barrier = decode_barrier(frame.payload);
    if (!barrier) {
        fail(barrier.error());
        return;
    }
    auto current = pause_.current(*barrier);
    if (!current) {
        fail(current.error());
        return;
    }
    if (!*current) {
        schedule();
        return;
    }
    if (pause_.paused() || !pause_.held() || io_busy_ || in_flight_ || barrier->bytes != durable_ ||
        (frame.type == Message::pause_barrier
             ? role_ != Role::receiver
             : role_ != Role::initiator || !pending_barrier_ || *pending_barrier_ != *barrier)) {
        fail(Error{ErrorCode::invalid_state});
        return;
    }
    if (frame.type == Message::pause_barrier && !send(Message::barrier_ack, frame.payload))
        return;
    pause_.acknowledge();
    pending_barrier_.reset();
    flow_changed();
}
void Transfer::verify() {
    phase_ = TransferPhase::verifying;
    io_busy_ = true;
    notify();
    auto submitted = worker_->submit(
        [](FileWorker::State& state, std::stop_token stop) -> Result<FileWorker::Value> {
            if (!state.partial->committed()) {
                auto done = state.partial->finish(stop);
                if (!done)
                    return std::unexpected(done.error());
            }
            if (state.folder) {
                auto published = state.folder->publish(stop);
                if (!published)
                    return std::unexpected(published.error());
            }
            return std::monostate{};
        },
        [this](Result<FileWorker::Value> result) {
            io_busy_ = false;
            if (terminal_)
                return;
            if (!result) {
                fail(result.error());
                return;
            }
            phase_ = TransferPhase::closing;
            if (!send(Message::file_committed, manifest_->id))
                return;
            auto close = session_.expect_close();
            if (!close) {
                fail(close.error());
                return;
            }
            notify();
        });
    if (!submitted)
        fail(submitted.error());
}
void Transfer::handle(const Frame& frame) {
    if (terminal_)
        return;
    if (frame.type == Message::heartbeat)
        return;
    switch (frame.type) {
    case Message::offer: {
        if (role_ != Role::receiver || phase_ != TransferPhase::awaiting_offer || manifest_)
            break;
        auto m = decode_offer(frame.payload);
        if (!m) {
            fail(m.error());
            return;
        }
        if (!io::validate_relative_path(m->name) || m->name.find('/') != std::string::npos) {
            fail(Error{ErrorCode::invalid_path});
            return;
        }
        manifest_ = std::move(*m);
        phase_ = TransferPhase::awaiting_accept;
        notify();
        return;
    }
    case Message::accept_prefix:
    case Message::durable_ack:
        receive_prefix(frame);
        return;
    case Message::data_chunk:
        receive_chunk(frame);
        return;
    case Message::hold: {
        if (!flow_phase() && !(role_ == Role::initiator && phase_ == TransferPhase::verifying))
            break;
        auto h = decode_hold(frame.payload);
        if (!h) {
            fail(h.error());
            return;
        }
        auto valid = pause_.receive(*h);
        if (!valid) {
            fail(valid.error());
            return;
        }
        pending_barrier_.reset();
        flow_changed();
        return;
    }
    case Message::pause_barrier:
    case Message::barrier_ack:
        receive_barrier(frame);
        return;
    case Message::finish_file:
        if (role_ != Role::receiver || !manifest_ || !flow_phase() || pause_.paused() || io_busy_ ||
            durable_ != manifest_->size || !matches_id(frame, *manifest_))
            break;
        verify();
        return;
    case Message::file_committed:
        if (role_ != Role::initiator || phase_ != TransferPhase::verifying || !manifest_ ||
            !matches_id(frame, *manifest_))
            break;
        phase_ = TransferPhase::closing;
        session_.close_file();
        notify();
        return;
    default:
        break;
    }
    fail(Error{ErrorCode::invalid_state});
}
void Transfer::cancel() {
    if (!terminal_)
        fail(Error{ErrorCode::cancelled});
}
void Transfer::fail(Error error) {
    if (terminal_)
        return;
    logger_.write(Event::transfer_failed, diagnostic_id_, static_cast<std::uint16_t>(error.code));
    terminal(false, error.code);
    session_.cancel();
}
void Transfer::terminal(bool success, ErrorCode code) {
    terminal_ = true;
    success_ = success;
    error_ = code;
    phase_ = success ? TransferPhase::complete : TransferPhase::failed;
    heartbeat_.stop();
    worker_->stop();
    notify();
}
void Transfer::notify() {
    if (phase_ != reported_phase_) {
        reported_phase_ = phase_;
        logger_.write(Event::transfer_state, diagnostic_id_, static_cast<std::uint64_t>(phase_));
    }
    if (durable_ != reported_prefix_) {
        reported_prefix_ = durable_;
        logger_.write(Event::durable_checkpoint, diagnostic_id_, durable_);
    }
    emit changed();
}
void Transfer::worker_stopped() {
    if (!terminal_ || finished_)
        return;
    finished_ = true;
    notify();
    emit finished(success_, static_cast<int>(error_));
}
} // namespace bridge::app
