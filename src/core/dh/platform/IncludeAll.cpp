// Compiles every platform contract header on both toolchains (host clang and
// clang-cl cross-build) so a header-only mistake surfaces in the normal build,
// not only when the first consumer appears. Keep in sync with dh/platform/*.h.

#include "dh/platform/BlockInfo.h"
#include "dh/platform/Direction.h"
#include "dh/platform/IBlockRegistry.h"
#include "dh/platform/IChunk.h"
#include "dh/platform/IChunkGenerator.h"
#include "dh/platform/IClock.h"
#include "dh/platform/IExecutor.h"
#include "dh/platform/IFileSystem.h"
#include "dh/platform/ILevelInfo.h"
#include "dh/platform/ILodStore.h"
#include "dh/platform/ILogger.h"
#include "dh/platform/ITransport.h"
#include "dh/platform/IVertexSink.h"
#include "dh/platform/testing/TestChunk.h"
#include "dh/thread/InlineExecutor.h"
#include "dh/thread/ManualExecutor.h"
#include "dh/util/Assert.h"
#include "dh/util/Result.h"

namespace dh {

// Instantiate a few templates / inline functions so they are actually compiled.
namespace {
[[maybe_unused]] void platformHeadersSmoke() {
    Result<int>         r = 1;
    Status              s = Status::ok();
    Result<std::string> e = Error{ErrorCode::Io, "x"};
    (void)r.value_or(0);
    (void)s.has_value();
    (void)e.has_value();
    static_assert(opposite(Direction::North) == Direction::South);
    static_assert(generationStepFromName("terrain") == GenerationStep::LiquidCarvers);
    static_assert(has(StorePart::All, StorePart::Meta));
    static_assert(hashSerial("minecraft:air") != hashSerial("minecraft:stone"));
    log::logf(LogLevel::Trace, "%d", 0);
}
} // namespace

} // namespace dh
