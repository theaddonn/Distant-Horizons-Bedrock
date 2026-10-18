#pragma once

// IExecutor that runs every task synchronously inside execute(). Useful for
// tests that do not care about scheduling and for single-threaded tools.

#include "dh/platform/IExecutor.h"

namespace dh {

class InlineExecutor final : public IExecutor {
public:
    bool execute(Task task) override {
        if (shutdown_) return false;
        if (task) task();
        return true;
    }
    std::size_t queueSize() const override { return 0; }

    void shutdown() { shutdown_ = true; }

private:
    bool shutdown_ = false;
};

} // namespace dh
