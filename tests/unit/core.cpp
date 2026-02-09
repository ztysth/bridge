#include "bridge/core/diagnostics.hpp"
#include "bridge/core/session.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <sstream>
using namespace bridge;
TEST_CASE("bounded frame roundtrip and hostile headers") {
    auto encoded = encode_frame(Message::ping, ping_payload);
    REQUIRE(encoded);
    const auto decoded = decode_frame(*encoded);
    REQUIRE(decoded);
    REQUIRE(decoded->type == Message::ping);
    REQUIRE(std::ranges::equal(decoded->payload, ping_payload));
    for (std::size_t size = 0; size < encoded->size(); ++size)
        REQUIRE_FALSE(decode_frame(std::span<const std::uint8_t>(*encoded).first(size)));
    auto header = std::vector<std::uint8_t>(encoded->begin(), encoded->begin() + 12);
    for (auto length : {4097U, std::numeric_limits<std::uint32_t>::max()}) {
        header[8] = static_cast<std::uint8_t>(length >> 24U);
        header[9] = static_cast<std::uint8_t>(length >> 16U);
        header[10] = static_cast<std::uint8_t>(length >> 8U);
        header[11] = static_cast<std::uint8_t>(length);
        REQUIRE(decode_header(header).error().code == ErrorCode::oversized_frame);
    }
    header.assign(encoded->begin(), encoded->begin() + 12);
    for (auto offset : {0U, 6U, 7U}) {
        auto bad = header;
        bad[offset] = 255;
        REQUIRE_FALSE(decode_header(bad));
    }
    header[5] = 2;
    REQUIRE(decode_header(header).error().code == ErrorCode::unsupported_version);
    REQUIRE_FALSE(encode_frame(Message::confirm, ping_payload));
    encoded->push_back(0);
    REQUIRE_FALSE(decode_frame(*encoded));
}
TEST_CASE("bilateral pairing and ordered clean lifecycle") {
    SessionState client(Role::initiator), server(Role::receiver);
    REQUIRE_FALSE(client.paired());
    REQUIRE(client.confirm()->messages[0] == Message::confirm);
    REQUIRE(server.receive(Frame{Message::confirm, {}})->count == 0);
    const auto response = server.confirm();
    REQUIRE(response);
    REQUIRE(server.paired());
    REQUIRE(client.receive(Frame{Message::confirm, {}})->messages[0] == Message::hello);
    REQUIRE(server.receive(Frame{Message::hello, {0, 0, 0, 0}})->messages[0] == Message::hello_ack);
    REQUIRE(client.receive(Frame{Message::hello_ack, {0, 0}})->messages[0] == Message::ping);
    REQUIRE(server.receive(Frame{Message::ping, {ping_payload.begin(), ping_payload.end()}})
                ->messages[0] == Message::pong);
    REQUIRE(client.receive(Frame{Message::pong, {ping_payload.begin(), ping_payload.end()}})
                ->messages[0] == Message::close);
    REQUIRE(server.receive(Frame{Message::close, {}})->shutdown);
    REQUIRE(client.receive(Frame{Message::close_ack, {}})->shutdown);
    REQUIRE(server.disconnected());
    REQUIRE(client.disconnected());
    REQUIRE(client.phase() == Phase::complete);
    REQUIRE_FALSE(client.confirm());
}
TEST_CASE("wrong state, duplicate confirmation, version and payload rejected") {
    SessionState server(Role::receiver);
    REQUIRE(server.receive(Frame{Message::confirm, {1}}).error().code ==
            ErrorCode::malformed_frame);
    REQUIRE_FALSE(server.receive(Frame{Message::cancel, {1}}));
    REQUIRE_FALSE(server.receive(Frame{Message::error, {}}));
    REQUIRE_FALSE(server.receive(Frame{Message::hello, {0, 0, 0, 0}}));
    REQUIRE(server.receive(Frame{Message::confirm, {}}));
    REQUIRE_FALSE(server.receive(Frame{Message::confirm, {}}));
    REQUIRE(server.confirm());
    REQUIRE(server.receive(Frame{Message::hello, {0, 1, 0, 1}}).error().code ==
            ErrorCode::unsupported_version);
    REQUIRE(server.receive(Frame{Message::hello, {0, 0, 0, 0}}));
    REQUIRE_FALSE(server.receive(Frame{Message::ping, std::vector<std::uint8_t>(16, 0)}));
    REQUIRE_FALSE(server.disconnected());
    SessionState cancelled(Role::initiator);
    REQUIRE(cancelled.receive(Frame{Message::cancel, {}}).error().code == ErrorCode::cancelled);
}
TEST_CASE("typed validation and diagnostics evaluate conditions once") {
    int count = 0;
    BRIDGE_CHECK(++count == 1);
    REQUIRE(count == 1);
    auto validate = [&count]() -> Result<void> {
        BRIDGE_ENSURE(++count == 3, ErrorCode::invalid_state);
        return {};
    };
    REQUIRE(validate().error().code == ErrorCode::invalid_state);
    REQUIRE(count == 2);
    Failpoint fault(2);
    REQUIRE_FALSE(fault.hit());
    REQUIRE(fault.hit());
    REQUIRE_FALSE(fault.hit());
    std::ostringstream output;
    Logger logger(output);
    logger.write(Event::session_started, 7);
    { BRIDGE_TRACE_SCOPE(logger, 7); }
    REQUIRE(
        output.str().starts_with("{\"event\":\"session_started\",\"session\":7,\"value\":0}\n"));
    REQUIRE(output.str().find("trace") != std::string::npos);
    REQUIRE(error_message(ErrorCode::timeout).find("timed out") != std::string_view::npos);
}
