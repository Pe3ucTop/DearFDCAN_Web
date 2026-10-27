#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <SDL.h>
#include <SDL_opengles2.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

// Forward declarations of C callbacks invoked asynchronously from JavaScript WebUSB promises
extern "C" {
    EMSCRIPTEN_KEEPALIVE void on_webusb_connected(int vid, int pid);
    EMSCRIPTEN_KEEPALIVE void on_webusb_disconnected();
    EMSCRIPTEN_KEEPALIVE void on_webusb_error(const char* err_msg);
    EMSCRIPTEN_KEEPALIVE void on_webusb_tx_complete(int ep, int bytes_sent, int status);
    EMSCRIPTEN_KEEPALIVE void on_webusb_rx_complete(int ep, const uint8_t* data, int length, int status);
}

// ============================================================================
// Asynchronous JavaScript WebUSB Bridge (Zero Asyncify, Pure Event Callbacks)
// ============================================================================

EM_JS(void, js_webusb_request_and_open, (int interface_num), {
    if (!navigator.usb) {
         SendError("WebUSB API is not supported in this browser.");
        return;
    }

    function SendError(msg) {
        const len = lengthBytesUTF8(msg) + 1;
        const ptr = _malloc(len);
        stringToUTF8(msg, ptr, len);
        _on_webusb_error(ptr);
        _free(ptr);
    }

    // Store active device on the global Module object
    navigator.usb.requestDevice({ filters: [] })
        .then(device => {
            Module.activeUsbDevice = device;
            return device.open();
        })
        .then(() => {
            const dev = Module.activeUsbDevice;
            if (dev.configuration === null) {
                return dev.selectConfiguration(1);
            }
        })
        .then(() => {
            return Module.activeUsbDevice.claimInterface(interface_num);
        })
        .then(() => {
            const dev = Module.activeUsbDevice;
            _on_webusb_connected(dev.vendorId, dev.productId);
        })
        .catch(err => {
            SendError(err.message || err.toString());
        });
});

EM_JS(void, js_webusb_disconnect, (int interface_num), {
    if (!Module.activeUsbDevice) return;
    const dev = Module.activeUsbDevice;
    Module.activeUsbDevice = null;

    dev.releaseInterface(interface_num)
        .catch(() => {})
        .then(() => dev.close())
        .then(() => {
            _on_webusb_disconnected();
        });
});

EM_JS(void, js_webusb_transfer_out, (int endpoint_num, const uint8_t* data_ptr, int length), {
    if (!Module.activeUsbDevice) return;

    // Copy payload from Wasm HEAPU8 into a standalone Uint8Array before async transfer
    const payload = HEAPU8.slice(data_ptr, data_ptr + length);
    const ep = endpoint_num & 0x7F; // WebUSB uses endpoint number without direction bit

    Module.activeUsbDevice.transferOut(ep, payload)
        .then(result => {
            const status = (result.status === "ok") ? 0 : 1;
            _on_webusb_tx_complete(endpoint_num, result.bytesWritten, status);
        })
        .catch(err => {
            const msg = "TX Error: " + (err.message || err.toString());
            const len = lengthBytesUTF8(msg) + 1;
            const ptr = _malloc(len);
            stringToUTF8(msg, ptr, len);
            _on_webusb_error(ptr);
            _free(ptr);
            _on_webusb_tx_complete(endpoint_num, 0, -1);
        });
});

EM_JS(void, js_webusb_transfer_in, (int endpoint_num, int max_length), {
    if (!Module.activeUsbDevice) return;

    const ep = endpoint_num & 0x7F; // Strip 0x80 IN direction bit for WebUSB API

    Module.activeUsbDevice.transferIn(ep, max_length)
        .then(result => {
            const status = (result.status === "ok") ? 0 : 1;
            if (result.data && result.data.byteLength > 0) {
                const byteLen = result.data.byteLength;
                const wasmBuf = _malloc(byteLen);
                const srcView = new Uint8Array(result.data.buffer, result.data.byteOffset, byteLen);
                HEAPU8.set(srcView, wasmBuf);
                _on_webusb_rx_complete(endpoint_num, wasmBuf, byteLen, status);
                _free(wasmBuf);
            } else {
                _on_webusb_rx_complete(endpoint_num, 0, 0, status);
            }
        })
        .catch(err => {
            const msg = "RX Error: " + (err.message || err.toString());
            const len = lengthBytesUTF8(msg) + 1;
            const ptr = _malloc(len);
            stringToUTF8(msg, ptr, len);
            _on_webusb_error(ptr);
            _free(ptr);
            _on_webusb_rx_complete(endpoint_num, 0, 0, -1);
        });
});

// ============================================================================
// C++ Application State & ImGui Interface
// ============================================================================

struct AppState {
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;

    bool is_connected = false;
    bool rx_pending = false;

    int interface_num = 0;
    int ep_out = 0x01;
    int ep_in = 0x81;

    char send_payload[256] = "Hello Event-Driven WebUSB!";
    std::vector<std::string> log_messages;

    void AddLog(const char* fmt, ...) {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        log_messages.push_back(std::string(buf));
    }
};

static AppState g_App;

// Implementation of C callbacks triggered by JavaScript Promises
extern "C" {

EMSCRIPTEN_KEEPALIVE void on_webusb_connected(int vid, int pid) {
    g_App.is_connected = true;
    g_App.AddLog("[USB] Connected to VID: 0x%04X, PID: 0x%04X (Interface %d)",
                 vid, pid, g_App.interface_num);
}

EMSCRIPTEN_KEEPALIVE void on_webusb_disconnected() {
    g_App.is_connected = false;
    g_App.rx_pending = false;
    g_App.AddLog("[USB] Device disconnected.");
}

EMSCRIPTEN_KEEPALIVE void on_webusb_error(const char* err_msg) {
    g_App.AddLog("[ERR] %s", err_msg);
}

EMSCRIPTEN_KEEPALIVE void on_webusb_tx_complete(int ep, int bytes_sent, int status) {
    if (status == 0) {
        g_App.AddLog("[TX -> EP 0x%02X] Sent %d bytes.", ep, bytes_sent);
    } else {
        g_App.AddLog("[TX -> EP 0x%02X] Transfer failed with status %d.", ep, status);
    }
}

EMSCRIPTEN_KEEPALIVE void on_webusb_rx_complete(int ep, const uint8_t* data, int length, int status) {
    g_App.rx_pending = false;
    if (status == 0 && length > 0 && data != nullptr) {
        std::string hex_str;
        std::string ascii_str;
        char tmp[8];

        for (int i = 0; i < length; ++i) {
            uint8_t b = data[i];
            snprintf(tmp, sizeof(tmp), "%02X ", b);
            hex_str += tmp;
            ascii_str += (b >= 32 && b <= 126) ? (char)b : '.';
        }
        g_App.AddLog("[RX <- EP 0x%02X] (%d B) HEX: %s| ASCII: %s",
                     ep, length, hex_str.c_str(), ascii_str.c_str());
    } else if (status == 0) {
        g_App.AddLog("[RX <- EP 0x%02X] Received empty packet (ZLP).", ep);
    }
}

} // extern "C"

// Main non-blocking render loop
void main_loop() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(15, 15), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(680, 480), ImGuiCond_FirstUseEver);

    ImGui::Begin("Pure Event-Driven WebUSB (No Asyncify / No SAB)");

    // --- Connection Bar ---
    if (!g_App.is_connected) {
        ImGui::PushItemWidth(100);
        ImGui::InputInt("Interface #", &g_App.interface_num);
        ImGui::PopItemWidth();
        ImGui::SameLine();

        // Trigger immediately on click to preserve Browser User Gesture token
        if (ImGui::Button("Request WebUSB Device", ImVec2(240, 28))) {
            g_App.AddLog("[SYS] Requesting device permission from browser...");
            js_webusb_request_and_open(g_App.interface_num);
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Disconnected");
    } else {
        if (ImGui::Button("Disconnect", ImVec2(150, 28))) {
            js_webusb_disconnect(g_App.interface_num);
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Connected");
    }

    ImGui::Separator();

    // --- Controls ---
    ImGui::BeginDisabled(!g_App.is_connected);

    ImGui::PushItemWidth(120);
    ImGui::InputInt("OUT EP (Hex)", &g_App.ep_out, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::InputInt("IN EP (Hex)", &g_App.ep_in, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::PopItemWidth();

    ImGui::InputText("Payload", g_App.send_payload, sizeof(g_App.send_payload));

    if (ImGui::Button("Send Packet (Async OUT)", ImVec2(200, 30))) {
        size_t len = strlen(g_App.send_payload);
        if (len > 0) {
            js_webusb_transfer_out(g_App.ep_out, (const uint8_t*)g_App.send_payload, (int)len);
        }
    }
    ImGui::SameLine();

    ImGui::BeginDisabled(g_App.rx_pending);
    if (ImGui::Button(g_App.rx_pending ? "Waiting for IN..." : "Read Endpoint (Async IN)", ImVec2(200, 30))) {
        g_App.rx_pending = true;
        js_webusb_transfer_in(g_App.ep_in, 512);
    }
    ImGui::EndDisabled();

    ImGui::EndDisabled();
    ImGui::Separator();

    // --- Log Window ---
    ImGui::Text("Endpoint Activity Log:");
    ImGui::SameLine(ImGui::GetWindowWidth() - 90);
    if (ImGui::SmallButton("Clear")) {
        g_App.log_messages.clear();
    }

    ImGui::BeginChild("LogRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : g_App.log_messages) {
        ImGui::TextUnformatted(line.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();

    // Render frame
    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(g_App.window);
}

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        return -1;
    }

    const char* glsl_version = "#version 300 es";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    g_App.window = SDL_CreateWindow(
        "Event-Driven WebUSB",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        720, 520,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    g_App.gl_context = SDL_GL_CreateContext(g_App.window);
    SDL_GL_MakeCurrent(g_App.window, g_App.gl_context);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(g_App.window, g_App.gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    g_App.AddLog("[SYS] Ready. Pure callback mode (No Asyncify, No SharedArrayBuffer).");

    emscripten_set_main_loop(main_loop, 0, true);
    return 0;
}