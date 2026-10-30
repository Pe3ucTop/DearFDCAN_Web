#include "clipboard.h"
#include "imgui.h"
#include <emscripten.h>

EM_JS(void, js_clip_install, (), {
    if (!Module.BrowserClipboard || Module.clipboard) return;
    Module.clipboard = Module.BrowserClipboard.create({ window: window, document: document, navigator: navigator, now: () => performance.now() });
    Module.clipboard.install();
});

EM_JS(void, js_clip_set, (const char* text), {
    if (Module.clipboard) Module.clipboard.set(UTF8ToString(text));
});

// Returns the length of the text in bytes; copies it (with the terminating zero) when `out` is given
EM_JS(int, js_clip_get, (char* out, int size), {
    const text = Module.clipboard ? Module.clipboard.get() : String();
    if (out && size > 0) stringToUTF8(text, out, size);
    return lengthBytesUTF8(text);
});

// 0 idle, 1 copied, 2 blocked
EM_JS(int, js_clip_state, (), {
    if (!Module.clipboard) return 0;
    const state = Module.clipboard.status().state;
    return state === 'copied' ? 1 : (state === 'blocked' ? 2 : 0);
});

EM_JS(double, js_clip_age_ms, (), {
    return Module.clipboard ? performance.now() - Module.clipboard.status().at : 0;
});

namespace BrowserClipboard {

namespace {

std::string g_paste;    // ImGui wants a pointer that stays valid until its next request

void SetText(ImGuiContext*, const char* text) {
    if (text) js_clip_set(text);
}

const char* GetText(ImGuiContext*) {
    const int n = js_clip_get(nullptr, 0);
    g_paste.assign(static_cast<size_t>(n) + 1, '\0');
    js_clip_get(&g_paste[0], n + 1);
    g_paste.resize(static_cast<size_t>(n));
    return g_paste.c_str();
}

constexpr double kCopiedShownMs = 2500.0;

}

void Install() {
    js_clip_install();
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Platform_SetClipboardTextFn = SetText;
    platform_io.Platform_GetClipboardTextFn = GetText;
}

State GetState(double* age_ms) {
    if (age_ms) *age_ms = js_clip_age_ms();
    switch (js_clip_state()) {
        case 1:  return State::Copied;
        case 2:  return State::Blocked;
        default: return State::Idle;
    }
}

std::string StatusText() {
    double age = 0.0;
    const State state = GetState(&age);
    if (state == State::Copied && age < kCopiedShownMs) return "Copied";
    if (state == State::Blocked) return "The browser refused: click again or press Ctrl+C";
    return std::string();
}

}
