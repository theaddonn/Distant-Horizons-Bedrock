// Debug HUD: text block top-left, drawn after the UI with MinecraftUIRenderContext::drawDebugText (the built-in
// debug font; needs no Font handle). Only installed when config client.debug.enabled is true; the key named by
// client.debug.hudKey (default numpad0) shows/hides it, Numpad . writes the registry as JSON to dh.log.
// The key/value registry itself (set / dumpJson) always works, overlay or not.

#include "client/hud/DebugHud.h"

#include "bedrock_common/ConfigBridge.h"
#include "bedrock_common/FileLogger.h"
#include "client/Input.h"
#include "client/Version.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "mc/client/gui/CaretMeasureData.h"
#include "mc/client/gui/TextAlignment.h"
#include "mc/client/gui/TextMeasureData.h"
#include "mc/deps/core/math/Color.h"
#include "mc/deps/input/RectangleArea.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace dh::hud {

namespace {

std::mutex                         gMutex;
std::map<std::string, std::string> gValues;
ll::event::ListenerPtr             gListener;
bool                               gVisible = true;
int                                gHudKey  = 0; // VK of client.debug.hudKey
double                             gFrameMs = 0.0;

std::string jsonEscape(std::string const& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            o += ' ';
        } else {
            o += c;
        }
    }
    return o;
}

// TextMeasureData / CaretMeasureData are all-const storage structs without constructors; build them through their
// known layout (see mc/client/gui/TextMeasureData.h).
struct TextMeasureRaw {
    float fontSize;
    float linePadding;
    bool  renderShadow;
    bool  showColorSymbol;
    bool  hideHyphen;
    int   alignment;
};
static_assert(sizeof(TextMeasureRaw) == sizeof(TextMeasureData), "TextMeasureData layout");

struct CaretMeasureRaw {
    int  position;
    bool shouldRender;
};
static_assert(sizeof(CaretMeasureRaw) == sizeof(CaretMeasureData), "CaretMeasureData layout");

void rect(MinecraftUIRenderContext& ctx, float x0, float y0, float x1, float y1, mce::Color const& c, float a) {
    RectangleArea r{};
    r._x0 = x0;
    r._x1 = x1;
    r._y0 = y0;
    r._y1 = y1;
    ctx.fillRectangle(r, c, a);
}

void text(MinecraftUIRenderContext& ctx, float x, float y, std::string line) {
    static mce::Color white{1.0f, 1.0f, 1.0f, 1.0f};
    TextMeasureRaw    tm{1.0f, 0.0f, true, false, false, static_cast<int>(ui::TextAlignment::Left)};
    CaretMeasureRaw   cm{0, false};
    RectangleArea     r{};
    r._x0 = x;
    r._x1 = x + 600.0f;
    r._y0 = y;
    r._y1 = y + 10.0f;
    ctx.drawDebugText(
        r,
        std::move(line),
        white,
        1.0f,
        ui::TextAlignment::Left,
        reinterpret_cast<TextMeasureData const&>(tm),
        reinterpret_cast<CaretMeasureData const&>(cm)
    );
}

void onRender(ll::event::AfterUIRenderEvent& e) {
    if (e.screenView().getScreenName() != "hud_screen") return;

    using clock      = std::chrono::steady_clock;
    static auto last = clock::now();
    auto        now  = clock::now();
    double      dt   = std::chrono::duration<double, std::milli>(now - last).count();
    last             = now;
    if (dt > 0.0 && dt < 1000.0) gFrameMs = gFrameMs == 0.0 ? dt : gFrameMs * 0.95 + dt * 0.05;

    if (gHudKey != 0 && dh::input::pressed(gHudKey)) gVisible = !gVisible;
    if (dh::input::pressed(0x6E)) DH_LOG_I("hud-dump: %s", dumpJson().c_str()); // VK_DECIMAL (Numpad .)

    if (!gVisible) return;

    std::vector<std::string> lines;
    char                     buf[128];
    snprintf(buf, sizeof(buf), "DH %s (%s)", dh::kModVersion, dh::kGitSha);
    lines.emplace_back(buf);
    snprintf(buf, sizeof(buf), "frame ms: %.2f (%.0f fps)", gFrameMs, gFrameMs > 0 ? 1000.0 / gFrameMs : 0.0);
    lines.emplace_back(buf);
    {
        std::lock_guard lock(gMutex);
        for (auto const& [k, v] : gValues) lines.push_back(k + ": " + v);
    }

    auto& ctx = e.uiRenderContext();
    rect(ctx, 2, 2, 210, 6.0f + 10.0f * static_cast<float>(lines.size()), mce::Color{0.f, 0.f, 0.f, 1.f}, 0.55f);
    float y = 4.0f;
    for (auto& l : lines) {
        text(ctx, 4.0f, y, std::move(l));
        y += 10.0f;
    }
    ctx.flushText(0.0f, std::nullopt);
}

} // namespace

void set(std::string const& key, std::string const& value) {
    std::lock_guard lock(gMutex);
    gValues[key] = value;
}

void set(std::string const& key, double value) {
    char b[64];
    snprintf(b, sizeof(b), "%.3f", value);
    set(key, std::string{b});
}

void set(std::string const& key, std::int64_t value) { set(key, std::to_string(value)); }

std::string dumpJson() {
    std::lock_guard lock(gMutex);
    std::string     o = "{";
    char            b[64];
    snprintf(b, sizeof(b), "%.3f", gFrameMs);
    o += "\"version\":\"" + std::string{dh::kModVersion} + "\",\"frame_ms\":" + b;
    for (auto const& [k, v] : gValues) o += ",\"" + jsonEscape(k) + "\":\"" + jsonEscape(v) + "\"";
    o += "}";
    return o;
}

double frameMs() { return gFrameMs; }

void install() {
    if (gListener) return;
    auto const& dbg = bedrock::ConfigBridge::instance().config().options().client.debug;
    if (!dbg.enabled) return; // no overlay, no key polling
    gHudKey   = dh::input::resolveKey(dbg.hudKey, "client.debug.hudKey");
    gVisible  = true;
    gListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::AfterUIRenderEvent>(onRender);
}

void uninstall() {
    if (gListener) ll::event::EventBus::getInstance().removeListener(gListener);
    gListener.reset();
}

} // namespace dh::hud
