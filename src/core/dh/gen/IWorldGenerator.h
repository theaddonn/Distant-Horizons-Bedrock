#pragma once

// Core-side world generator abstraction: the part of Java
// api/.../override/worldGenerator/IDhApiWorldGenerator.java that
// WorldGenerationQueue consumes.
//
// Java offers three return types (vanilla chunks, API chunks, API data sources);
// DH's own DhWorldGenerator uses API_DATA_SOURCES, so that is the one shape we
// port: the generator fills a FullDataSource for one section and reports it via
// `onDone`. Chunk -> FullDataSource conversion (LodDataBuilder) is the
// generator implementation's job (WP-J2), not the queue's.
//
// Contract:
//   * generate() is non-blocking. Work runs on `genPool` (any thread). `onChunk`
//     may be called once per generated chunk (optional progress hook, mirrors
//     Java's resultConsumer); then `onDone` is called exactly once, on whatever
//     thread finished the work, with the data source or an Error
//     (ErrorCode::Cancelled when aborted by close()).
//   * After close() returns no callback may fire anymore: every outstanding
//     onDone has either fired (with Cancelled) or never will. The queue relies
//     on this for its own destruction.
//   * GenQueue never calls generate() once its shutdown has begun (it re-checks
//     right before each call), so an implementation does not see generate()
//     after close(). Defensive implementations may still answer such a call with
//     an immediate ErrorCode::Cancelled (FakeWorldGenerator does).
//   * Detail levels are absolute data detail levels (0 = block), as in Java:
//     smallestDataDetailLevel() <= requested <= largestDataDetailLevel().

#include <cstdint>
#include <functional>
#include <memory>

#include "dh/platform/IChunkGenerator.h"
#include "dh/util/Result.h"

namespace dh {

struct FullDataSource;
struct IExecutor;
class IChunk;

/// One generation request (Java generateLod(...) parameters).
struct GenerateRequest {
    std::uint64_t  sectionPos  = 0; ///< the section (LOD) being generated
    std::int32_t   minChunkX   = 0; ///< chunk X closest to negative infinity
    std::int32_t   minChunkZ   = 0;
    std::int32_t   widthChunks = 1; ///< widthChunks x widthChunks chunks are covered
    std::int8_t    dataDetail  = 0; ///< absolute data detail (sectionDetail - SECTION_MINIMUM_DETAIL_LEVEL)
    GenerationMode mode        = GenerationMode::Features;
};

struct IWorldGenerator {
    using ChunkCallback = std::function<void(IChunk&)>;
    using DoneCallback  = std::function<void(Result<std::shared_ptr<FullDataSource>>)>;

    virtual ~IWorldGenerator() = default;

    /// Java getSmallestDataDetailLevel (max detail, smallest number; default 0).
    virtual std::int8_t smallestDataDetailLevel() const = 0;
    /// Java getLargestDataDetailLevel (min detail, largest number; default 0).
    virtual std::int8_t largestDataDetailLevel() const = 0;

    virtual void
    generate(const GenerateRequest& request, IExecutor& genPool, ChunkCallback onChunk, DoneCallback onDone) = 0;

    /// Back-pressure hint (IChunkGenerator::isBusy). Java's API generator has no such
    /// method, so Java behaviour corresponds to always returning false.
    virtual bool isBusy() const = 0;

    /// Java preGeneratorTaskStart: called before each batch of task starts.
    virtual void preGeneratorTaskStart() = 0;

    /// Java close(): abort outstanding work (see contract above).
    virtual void close() = 0;
};

} // namespace dh
