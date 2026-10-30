#pragma once
#include <string>

// The browser's clipboard for Dear ImGui (the Emscripten GLFW has none: its clipboard functions do nothing). The work is done by
// libs/browser-clipboard/browser_clipboard.js, which is linked into the page.
namespace BrowserClipboard {

// Installs the browser listeners and makes ImGui use them (call after ImGui_ImplGlfw_InitForOpenGL, which sets its own functions)
void Install();

enum class State { Idle, Copied, Blocked };   // Blocked: the browser refused; the text is written at the next click or key press
State GetState(double* age_ms = nullptr);     // age_ms: time since the state changed

// What to tell the user next to a Copy button: "Copied", the request to click again, or "" (nothing to say or too long ago)
std::string StatusText();

}
