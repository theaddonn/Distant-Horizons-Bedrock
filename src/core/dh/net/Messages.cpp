#include "dh/net/Messages.h"

namespace dh::net {

using namespace NetCodec;

//--- trackable ---------------------------------------------------------------

void TrackableMessage::encode(ByteWriter& w) const {
    w.putU32BE(FutureId::wire(futureId));
    encodeBody(w);
}

bool TrackableMessage::decode(ByteReader& r) {
    auto v = r.getU32BE();
    if (!v) return false;
    futureId = FutureId::withWire(futureId, *v);
    return decodeBody(r);
}

//--- payload -----------------------------------------------------------------

void FullDataPayload::encode(ByteWriter& w) const {
    w.putI32BE(dtoBufferId);
    putList(w, beaconBeams, [](ByteWriter& ww, const BeaconBeam& b) {
        ww.putI32BE(b.x);
        ww.putI32BE(b.y);
        ww.putI32BE(b.z);
        ww.putI32BE(b.argb);
    });
}

bool FullDataPayload::decode(ByteReader& r) {
    return getI32(r, dtoBufferId) && getList(r, beaconBeams, 16, [](ByteReader& rr, BeaconBeam& b) {
               return getI32(rr, b.x) && getI32(rr, b.y) && getI32(rr, b.z) && getI32(rr, b.argb);
           });
}

//--- simple messages -----------------------------------------------------------

void CloseReasonMessage::encode(ByteWriter& w) const { putString(w, reason); }
bool CloseReasonMessage::decode(ByteReader& r) { return getString(r, reason); }

namespace {
bool isKeyChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}
bool isKeyPart(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!isKeyChar(c)) return false;
    return true;
}
} // namespace

bool isValidServerKey(std::string_view s) { return s.size() <= kMaxLevelKeyChars && isKeyPart(s); }

bool isValidLevelKey(std::string_view s) {
    if (s.empty() || s.size() > kMaxLevelKeyChars) return false;
    if (auto at = s.find('@'); at != std::string_view::npos) {
        if (!isKeyPart(s.substr(0, at))) return false;
        s.remove_prefix(at + 1);
    }
    if (auto colon = s.find(':'); colon != std::string_view::npos) {
        return isKeyPart(s.substr(0, colon)) && isKeyPart(s.substr(colon + 1));
    }
    return isKeyPart(s);
}

void LevelInitMessage::encode(ByteWriter& w) const {
    putString(w, dimensionResourceLocation);
    putString(w, serverKey);
    putString(w, levelKey);
    w.putI64BE(serverTime);
}

bool LevelInitMessage::decode(ByteReader& r) {
    if (!getString(r, dimensionResourceLocation) || !getString(r, serverKey) || !getString(r, levelKey)
        || !getI64(r, serverTime))
        return false;
    if (dimensionResourceLocation.size() > kMaxLevelKeyChars) return false;
    if (!serverKey.empty() && !isValidServerKey(serverKey)) return false;
    return isValidLevelKey(levelKey);
}

void RequestLevelInitMessage::encode(ByteWriter& w) const { putString(w, dimensionResourceLocation); }
bool RequestLevelInitMessage::decode(ByteReader& r) { return getString(r, dimensionResourceLocation); }

//--- trackable messages ----------------------------------------------------------

void ExceptionMessage::encodeBody(ByteWriter& w) const {
    w.putI32BE(static_cast<std::int32_t>(kind));
    putString(w, message);
}

bool ExceptionMessage::decodeBody(ByteReader& r) {
    std::int32_t idx = 0;
    if (!getI32(r, idx) || idx < 0 || idx >= kRemoteExceptionCount) return false;
    kind = static_cast<RemoteException>(idx);
    return getString(r, message);
}

void FullDataSourceRequestMessage::encodeBody(ByteWriter& w) const {
    putString(w, levelName);
    w.putI64BE(sectionPos);
    putOptional(w, clientTimestamp, [](ByteWriter& ww, std::int64_t v) { ww.putI64BE(v); });
}

bool FullDataSourceRequestMessage::decodeBody(ByteReader& r) {
    return getString(r, levelName) && getI64(r, sectionPos)
        && getOptional(r, clientTimestamp, [](ByteReader& rr, std::int64_t& v) { return getI64(rr, v); });
}

void FullDataSourceResponseMessage::encodeBody(ByteWriter& w) const {
    putOptional(w, payload, [](ByteWriter& ww, const FullDataPayload& p) { p.encode(ww); });
}

bool FullDataSourceResponseMessage::decodeBody(ByteReader& r) {
    return getOptional(r, payload, [](ByteReader& rr, FullDataPayload& p) { return p.decode(rr); });
}

//--- full data -----------------------------------------------------------------

void FullDataPartialUpdateMessage::encode(ByteWriter& w) const {
    putString(w, levelName);
    payload.encode(w);
}
bool FullDataPartialUpdateMessage::decode(ByteReader& r) { return getString(r, levelName) && payload.decode(r); }

void FullDataSplitMessage::encode(ByteWriter& w) const {
    w.putI32BE(bufferId);
    w.putI32BE(static_cast<std::int32_t>(buffer.size()));
    w.putBytes(buffer);
    putBool(w, isFirst);
}

bool FullDataSplitMessage::decode(ByteReader& r) {
    std::int32_t size = 0;
    if (!getI32(r, bufferId) || !getI32(r, size) || size < 0) return false;
    auto bytes = r.getBytes(static_cast<std::size_t>(size));
    if (!bytes) return false;
    buffer.assign(bytes->begin(), bytes->end());
    return getBool(r, isFirst);
}

} // namespace dh::net
