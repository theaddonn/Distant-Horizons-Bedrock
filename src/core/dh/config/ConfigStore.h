#pragma once

// JSON config store. No filesystem access: the glue reads/writes the file text.
//
// JSON layout: {"version":1,"client":{"graphics":{"quality":{...}}},"common":{...},"server":{...}}
// (nested by the dot-separated option path). Enums are their Java constant names ("MEDIUM").
// load(): unknown keys are ignored, missing keys keep their default, wrong-typed or unknown-enum
// values fall back to the default (a message is added to warnings()), numbers are clamped.

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dh/config/Options.h"

namespace dh {

class ConfigStore {
public:
    static constexpr int kVersion = 1;

    /// `cores` = logical processor count (defaults, thread preset and the numberOfThreads max
    /// depend on it). 0 -> std::thread::hardware_concurrency(), or 4 when that is unknown.
    explicit ConfigStore(int cores = 0);

    /// Replace all options with the parsed file (starting from defaults). Returns an error message
    /// (and leaves the options untouched) if the text is not a JSON object; otherwise nullopt,
    /// listeners fire once. Soft problems go to warnings().
    std::optional<std::string> load(std::string_view jsonText);

    /// Pretty-printed JSON of all options plus "version". Deterministic (keys sorted).
    std::string save() const;

    Options&       options() { return options_; }
    const Options& options() const { return options_; }
    int            cores() const { return cores_; }

    /// Listeners run (in registration order) from notify(), set() and load().
    void onChange(std::function<void(const Options&)> listener);

    /// Edit through `mutate`, clamp, then notify.
    void set(const std::function<void(Options&)>& mutate);
    /// Replace the options, clamp, then notify.
    void set(const Options& value);
    /// Clamp the current options and call all listeners (use after editing options() directly).
    void notify();

    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    Options                                          options_;
    int                                              cores_;
    std::vector<std::function<void(const Options&)>> listeners_;
    std::vector<std::string>                         warnings_;
};

} // namespace dh
