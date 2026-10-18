#include "dh/net/MessageRegistry.h"

#include "dh/net/Messages.h"

#include <string>

namespace dh::net {

std::unique_ptr<Message> create(MsgId id) {
    switch (id) {
    case MsgIds::CloseReason:
        return std::make_unique<CloseReasonMessage>();
    case MsgIds::LevelInit:
        return std::make_unique<LevelInitMessage>();
    case MsgIds::RequestLevelInit:
        return std::make_unique<RequestLevelInitMessage>();
    case MsgIds::SessionConfig:
        return std::make_unique<SessionConfigMessage>();
    case MsgIds::Cancel:
        return std::make_unique<CancelMessage>();
    case MsgIds::Exception:
        return std::make_unique<ExceptionMessage>();
    case MsgIds::FullDataSourceRequest:
        return std::make_unique<FullDataSourceRequestMessage>();
    case MsgIds::FullDataSourceResponse:
        return std::make_unique<FullDataSourceResponseMessage>();
    case MsgIds::FullDataPartialUpdate:
        return std::make_unique<FullDataPartialUpdateMessage>();
    case MsgIds::FullDataSplit:
        return std::make_unique<FullDataSplitMessage>();
    default:
        return nullptr;
    }
}

Result<std::unique_ptr<Message>> decodeFrame(std::span<const std::byte> frame) {
    ByteReader r(frame);
    auto       version = r.getU16BE();
    if (!version) return makeError(ErrorCode::Corrupt, "frame too short for protocol version");
    if (*version != kProtocolVersion)
        return makeError(ErrorCode::Unsupported, "protocol version mismatch: " + std::to_string(*version));
    auto id = r.getU16BE();
    if (!id) return makeError(ErrorCode::Corrupt, "frame too short for message id");
    std::unique_ptr<Message> msg = create(*id);
    if (!msg) return makeError(ErrorCode::Unsupported, "unknown message id: " + std::to_string(*id));
    if (!msg->decode(r)) return makeError(ErrorCode::Corrupt, "malformed message body, id " + std::to_string(*id));
    if (!r.atEnd()) return makeError(ErrorCode::Corrupt, "buffer has not been fully read");
    return Result<std::unique_ptr<Message>>(std::move(msg));
}

std::vector<std::byte> encodeFrame(const Message& message) {
    std::vector<std::byte> out;
    ByteWriter             w(out);
    w.putU16BE(kProtocolVersion);
    w.putU16BE(message.id());
    message.encode(w);
    return out;
}

} // namespace dh::net
