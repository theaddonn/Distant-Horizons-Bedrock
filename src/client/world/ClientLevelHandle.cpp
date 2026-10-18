#include "client/world/ClientLevelHandle.h"

namespace dh::bedrock {

ClientLevelHandle::ClientLevelHandle(
    Dimension&      dimension,
    int             dimensionId,
    dh::LevelInfo   info,
    dh::BlockTable& table,
    bool            clientSide
)
: dimension_(&dimension),
  dimensionId_(dimensionId),
  clientSide_(clientSide),
  raw_(info),
  level_(table, std::move(info)) {
    identifiers_.push_back(raw_.dimensionName);
    current_.store(&identifiers_.back());
}

std::string_view ClientLevelHandle::dhIdentifier() const { return *current_.load(); }

std::string ClientLevelHandle::levelKey() const {
    std::lock_guard lock(mutex_);
    return levelKey_.empty() ? std::string(*current_.load()) : levelKey_;
}

void ClientLevelHandle::setLevelKey(const std::string& levelKey) {
    if (levelKey.empty() || !clientSide_ || paired_ != nullptr || locked_.load()) return;
    std::lock_guard lock(mutex_);
    if (locked_.load()) return;
    levelKey_ = levelKey;
    identifiers_.push_back(levelKey);
    current_.store(&identifiers_.back());
    keyed_.store(true);
}

} // namespace dh::bedrock
