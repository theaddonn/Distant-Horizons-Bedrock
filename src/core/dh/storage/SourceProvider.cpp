#include "dh/storage/SourceProvider.h"

#include "dh/platform/ILogger.h"
#include "dh/pos/SectionPos.h"

#include <algorithm>
#include <utility>

namespace dh {

namespace {

constexpr std::size_t kWeakSweepThreshold = 4096;

void logPos(LogLevel level, const char* what, std::uint64_t pos, const Error& e) {
    log::logf(
        level,
        "%s at section [%d,%d,%d]: %s",
        what,
        static_cast<int>(sec::detail(pos)),
        static_cast<int>(sec::x(pos)),
        static_cast<int>(sec::z(pos)),
        e.message.c_str()
    );
}

} // namespace

SourceProvider::SourceProvider(
    ILodStore&             store,
    IBlockRegistry&        registry,
    IClock&                clock,
    IExecutor&             fileHandler,
    const CodecOptions&    codec,
    const ProviderOptions& options
)
: store_(store),
  registry_(registry),
  clock_(clock),
  fileHandler_(fileHandler),
  codec_(codec),
  options_(options) {
    UpdaterOptions uo;
    uo.update        = options_.update;
    uo.maxSkyLight   = options_.maxSkyLight;
    uo.codec         = codec_;
    uo.asyncExecutor = options_.updateExecutor != nullptr ? options_.updateExecutor : &fileHandler_;
    updater_         = std::make_unique<SourceUpdater>(*this, uo);
}

void SourceProvider::touchStrongLocked(const std::shared_ptr<const FullDataSource>& s) {
    if (options_.strongCacheSize == 0) return;
    auto it = std::find(strong_.begin(), strong_.end(), s);
    if (it != strong_.end()) strong_.erase(it);
    strong_.push_back(s);
    while (strong_.size() > options_.strongCacheSize) strong_.pop_front();
}

std::shared_ptr<FullDataSource> SourceProvider::loadFresh(std::uint64_t pos) {
    if (shutdown_.load()) return nullptr;

    auto rec = store_.get(pos, StorePart::All);
    if (!rec) {
        if (rec.error().code == ErrorCode::Corrupt) {
            logPos(LogLevel::Warn, "Corrupt record erased", pos, rec.error());
            (void)store_.erase(pos);
        } else {
            logPos(LogLevel::Warn, "Store read failed", pos, rec.error());
        }
        return nullptr;
    }
    if (!rec->has_value()) return std::make_shared<FullDataSource>(FullDataSource::createEmpty(pos));

    auto decoded = decode(**rec, registry_);
    if (!decoded) {
        if (decoded.error().code == ErrorCode::Corrupt) {
            // Java: tryLogCorruptedDataError + repo.deleteWithKey so the section can be regenerated.
            logPos(LogLevel::Warn, "Corrupt data erased", pos, decoded.error());
            (void)store_.erase(pos);
        } else {
            logPos(LogLevel::Warn, "Unable to decode", pos, decoded.error());
        }
        return nullptr;
    }
    return std::make_shared<FullDataSource>(std::move(decoded.value()));
}

std::shared_ptr<const FullDataSource> SourceProvider::get(std::uint64_t pos) {
    if (shutdown_.load()) return nullptr;

    std::uint64_t epochBefore = 0;
    {
        std::lock_guard lock(cacheMutex_);
        auto            it = weak_.find(pos);
        if (it != weak_.end()) {
            if (auto alive = it->second.lock()) {
                touchStrongLocked(alive);
                return alive;
            }
            weak_.erase(it);
        }
        epochBefore = epoch_;
    }

    std::shared_ptr<const FullDataSource> loaded = loadFresh(pos);
    if (!loaded) return nullptr;

    std::lock_guard lock(cacheMutex_);
    // Someone else cached this pos while we were loading: share theirs.
    auto it = weak_.find(pos);
    if (it != weak_.end()) {
        if (auto alive = it->second.lock()) {
            touchStrongLocked(alive);
            return alive;
        }
    }
    // A publish/invalidate happened since we started: our copy may be stale, hand it out uncached.
    if (epoch_ != epochBefore) return loaded;

    if (weak_.size() >= kWeakSweepThreshold) {
        for (auto w = weak_.begin(); w != weak_.end();) {
            if (w->second.expired()) w = weak_.erase(w);
            else ++w;
        }
    }
    weak_[pos] = loaded;
    touchStrongLocked(loaded);
    return loaded;
}

void SourceProvider::getAsync(std::uint64_t pos, std::function<void(std::shared_ptr<const FullDataSource>)> cb) {
    if (shutdown_.load()) {
        cb(nullptr);
        return;
    }
    // The callback is shared so a rejected execute() can still deliver nullptr (the task is dropped).
    auto shared = std::make_shared<std::function<void(std::shared_ptr<const FullDataSource>)>>(std::move(cb));
    if (!fileHandler_.execute([this, pos, shared] { (*shared)(get(pos)); })) (*shared)(nullptr);
}

Result<std::shared_ptr<FullDataSource>> SourceProvider::getAdj(std::uint64_t pos, Direction dir) {
    if (shutdown_.load()) return Error(ErrorCode::Rejected, "provider closed");
    if (compassIndex(dir) < 0) return Error(ErrorCode::InvalidArgument, "getAdj needs a horizontal direction");

    const StorePart parts =
        StorePart::Adjacent | StorePart::GenSteps | StorePart::CompressionModes | StorePart::Mapping | StorePart::Meta;
    auto rec = store_.get(pos, parts);
    if (!rec) return rec.error();
    if (!rec->has_value()) return std::make_shared<FullDataSource>(FullDataSource::createEmpty(pos));

    auto decoded = decodeAdjacentOnly(**rec, dir, registry_);
    if (!decoded) {
        if (decoded.error().code == ErrorCode::Corrupt) {
            logPos(LogLevel::Warn, "Corrupt adjacent data erased", pos, decoded.error());
            (void)store_.erase(pos);
            invalidate(pos);
        }
        return decoded.error();
    }
    return std::make_shared<FullDataSource>(std::move(decoded.value()));
}

std::optional<std::int64_t> SourceProvider::timestamp(std::uint64_t pos) {
    if (shutdown_.load()) return std::nullopt;
    auto t = store_.timestamp(pos);
    if (!t) return std::nullopt;
    return *t;
}

Status SourceProvider::setFlag(std::uint64_t pos, StoreFlag flag, bool value) {
    Status st = store_.setFlag(pos, flag, value);
    invalidate(pos);
    return st;
}

void SourceProvider::invalidate(std::uint64_t pos) {
    std::lock_guard lock(cacheMutex_);
    ++epoch_;
    weak_.erase(pos);
    std::erase_if(strong_, [pos](const std::shared_ptr<const FullDataSource>& s) { return s->pos == pos; });
}

void SourceProvider::publish(std::shared_ptr<const FullDataSource> src) {
    if (!src) return;
    std::lock_guard lock(cacheMutex_);
    ++epoch_;
    const std::uint64_t pos = src->pos;
    std::erase_if(strong_, [pos](const std::shared_ptr<const FullDataSource>& s) { return s->pos == pos; });
    weak_[pos] = src;
    touchStrongLocked(src);
}

void SourceProvider::close() {
    shutdown_.store(true);
    updater_->close();
    std::lock_guard lock(cacheMutex_);
    strong_.clear();
    weak_.clear();
}

} // namespace dh
