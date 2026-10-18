#pragma once

// One struct per Java network message (field order/types identical => byte-identical bodies).
// See MessageRegistry.h for the frame envelope and ids.

#include "dh/net/Message.h"
#include "dh/net/SessionConfig.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dh::net {

namespace MsgIds {
inline constexpr MsgId CloseReason            = 1;
inline constexpr MsgId LevelInit              = 2;
inline constexpr MsgId RequestLevelInit       = 3;
inline constexpr MsgId SessionConfig          = 4;
inline constexpr MsgId Cancel                 = 5;
inline constexpr MsgId Exception              = 6;
inline constexpr MsgId FullDataSourceRequest  = 7;
inline constexpr MsgId FullDataSourceResponse = 8;
inline constexpr MsgId FullDataPartialUpdate  = 9;
inline constexpr MsgId FullDataSplit          = 10;
// Java also registers CodecCrashMessage as 11 in dev builds only; deliberately not ported.
inline constexpr MsgId Last = FullDataSplit;
} // namespace MsgIds

//------------------------------------------------------------------------------
// Trackable base (AbstractTrackableMessage)
//------------------------------------------------------------------------------

namespace FutureId {
/// Java computed `((env==SERVER_ONLY?1:0) << 31)` on an int: harmless as a bit but sign-extending, and the
/// session id was OR'd in with a sign-extended decoded int. Fixed layout (64 bits):
///   [63..32] session id (local only, never on the wire)
///   [31]     requesting side (0 client, 1 server)
///   [30..0]  per-process counter
constexpr std::uint64_t make(std::uint32_t sessionId, bool serverSide, std::uint32_t counter) {
    return (static_cast<std::uint64_t>(sessionId) << 32) | (static_cast<std::uint64_t>(serverSide ? 1u : 0u) << 31)
         | static_cast<std::uint64_t>(counter & 0x7FFFFFFFu);
}
constexpr std::uint32_t wire(std::uint64_t futureId) { return static_cast<std::uint32_t>(futureId); }
/// Replaces the low 32 bits (what the wire carries) and keeps the local session id.
constexpr std::uint64_t withWire(std::uint64_t futureId, std::uint32_t wireBits) {
    return (futureId & 0xFFFFFFFF00000000ull) | wireBits;
}
} // namespace FutureId

/// Wire: i32 BE (low 32 bits of futureId) then the body.
struct TrackableMessage : Message {
    std::uint64_t futureId = 0;

    void encode(ByteWriter& w) const final;
    bool decode(ByteReader& r) final;

    /// Responses reuse the request's id.
    void setSession(std::uint32_t sessionId) {
        futureId = (futureId & 0xFFFFFFFFull) | (std::uint64_t(sessionId) << 32);
    }

protected:
    virtual void encodeBody(ByteWriter&) const = 0;
    virtual bool decodeBody(ByteReader&)       = 0;
};

//------------------------------------------------------------------------------
// Shared payload types
//------------------------------------------------------------------------------

/// BeaconBeamDTO: i32 x, i32 y, i32 z, i32 argb (16 bytes).
struct BeaconBeam {
    std::int32_t x = 0, y = 0, z = 0;
    std::int32_t argb                                             = 0;
    friend bool  operator==(const BeaconBeam&, const BeaconBeam&) = default;
};

/// FullDataPayload: i32 dtoBufferId, i32 count, count * BeaconBeam. The data itself follows in FullDataSplit frames.
struct FullDataPayload {
    std::int32_t            dtoBufferId = 0;
    std::vector<BeaconBeam> beaconBeams;
    friend bool             operator==(const FullDataPayload&, const FullDataPayload&) = default;

    void encode(ByteWriter& w) const;
    bool decode(ByteReader& r);
};

//------------------------------------------------------------------------------
// Messages
//------------------------------------------------------------------------------

/// string reason
struct CloseReasonMessage : Message {
    std::string reason;
    MsgId       id() const override { return MsgIds::CloseReason; }
    void        encode(ByteWriter& w) const override;
    bool        decode(ByteReader& r) override;
};

inline constexpr std::size_t kMaxLevelKeyChars = 150;
/// SERVER_KEY_REGEX: 1-150 chars of [a-zA-Z0-9-_].
bool isValidServerKey(std::string_view s);
/// LEVEL_KEY_REGEX: `(prefix@)?name(:path)?`, each part [a-zA-Z0-9-_]+, 1-150 chars in total.
bool isValidLevelKey(std::string_view s);

/// string dimensionResourceLocation, string serverKey, string levelKey, i64 serverTime
/// decode validates (as Java's ClientPluginChannelApi does after decoding): dimensionResourceLocation <= 150
/// bytes; serverKey empty or a valid server key; levelKey a valid level key.
struct LevelInitMessage : Message {
    std::string  dimensionResourceLocation;
    std::string  serverKey;
    std::string  levelKey;
    std::int64_t serverTime = 0;
    MsgId        id() const override { return MsgIds::LevelInit; }
    void         encode(ByteWriter& w) const override;
    bool         decode(ByteReader& r) override;
};

/// string dimensionResourceLocation
struct RequestLevelInitMessage : Message {
    std::string dimensionResourceLocation;
    MsgId       id() const override { return MsgIds::RequestLevelInit; }
    void        encode(ByteWriter& w) const override;
    bool        decode(ByteReader& r) override;
};

/// SessionConfigValues (see SessionConfig.h)
struct SessionConfigMessage : Message {
    SessionConfigValues config;
    MsgId               id() const override { return MsgIds::SessionConfig; }
    void                encode(ByteWriter& w) const override { config.encode(w); }
    bool                decode(ByteReader& r) override { return config.decode(r); }
};

/// trackable, empty body
struct CancelMessage : TrackableMessage {
    MsgId id() const override { return MsgIds::Cancel; }

protected:
    void encodeBody(ByteWriter&) const override {}
    bool decodeBody(ByteReader&) override { return true; }
};

/// Java ExceptionMessage.EXCEPTION_LIST order.
enum class RemoteException : std::uint8_t {
    RateLimited = 0,
    RequestOutOfRange,
    RequestRejected,
    SectionRequiresSplitting,
};
inline constexpr std::uint8_t kRemoteExceptionCount = 4;

/// trackable; i32 exceptionIndex, string message
struct ExceptionMessage : TrackableMessage {
    RemoteException kind = RemoteException::RequestRejected;
    std::string     message;
    MsgId           id() const override { return MsgIds::Exception; }

protected:
    void encodeBody(ByteWriter& w) const override;
    bool decodeBody(ByteReader& r) override;
};

/// trackable; string levelName, i64 sectionPos, optional<i64> clientTimestamp
struct FullDataSourceRequestMessage : TrackableMessage {
    std::string                 levelName;
    std::int64_t                sectionPos = 0;
    std::optional<std::int64_t> clientTimestamp;
    MsgId                       id() const override { return MsgIds::FullDataSourceRequest; }

protected:
    void encodeBody(ByteWriter& w) const override;
    bool decodeBody(ByteReader& r) override;
};

/// trackable; optional<FullDataPayload>
struct FullDataSourceResponseMessage : TrackableMessage {
    std::optional<FullDataPayload> payload;
    MsgId                          id() const override { return MsgIds::FullDataSourceResponse; }

protected:
    void encodeBody(ByteWriter& w) const override;
    bool decodeBody(ByteReader& r) override;
};

/// string levelName, FullDataPayload
struct FullDataPartialUpdateMessage : Message {
    std::string     levelName;
    FullDataPayload payload;
    MsgId           id() const override { return MsgIds::FullDataPartialUpdate; }
    void            encode(ByteWriter& w) const override;
    bool            decode(ByteReader& r) override;
};

/// i32 bufferId, i32 size, size bytes (opaque, our own codec), bool isFirst
struct FullDataSplitMessage : Message {
    std::int32_t           bufferId = 0;
    std::vector<std::byte> buffer;
    bool                   isFirst = false;
    MsgId                  id() const override { return MsgIds::FullDataSplit; }
    void                   encode(ByteWriter& w) const override;
    bool                   decode(ByteReader& r) override;
};

} // namespace dh::net
