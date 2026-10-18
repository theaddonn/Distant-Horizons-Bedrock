#pragma once

// Frame envelope + message registry (MessageRegistry.java, AbstractPluginPacketSender.java).
//
// Frame (all big-endian), identical to Java except the version value:
//   u16 protocolVersion | u16 messageId | message body (to end of frame, no length, no padding)
// ITransport delivers whole frames, so the body ends at the end of the span; any unread byte is an error
// (Java: "Buffer has not been fully read").
//
// Ids are assigned by registration order starting at 1 (see MsgIds in Messages.h).

#include "dh/net/Message.h"
#include "dh/util/Result.h"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace dh::net {

/// Java PROTOCOL_VERSION is 16. Bit 14 is set so stock Java DH never accepts our frames (its check is
/// `version != 16`) and we never accept its frames; the full-data payload codec differs anyway.
inline constexpr std::uint16_t kJavaProtocolVersion = 16;
inline constexpr std::uint16_t kProtocolVersion     = 0x4000 | kJavaProtocolVersion;

/// New empty message for an id, or nullptr when the id is not registered.
std::unique_ptr<Message> create(MsgId id);

/// Errors: Unsupported (wrong version, unknown id), Corrupt (truncated / invalid field / trailing bytes).
Result<std::unique_ptr<Message>> decodeFrame(std::span<const std::byte> frame);

std::vector<std::byte> encodeFrame(const Message& message);

} // namespace dh::net
