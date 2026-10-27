#include <GLFW/glfw3.h>
#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <cstdio>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "webusb_bridge.h"
#include "can_protocol.h"
#include "app_ui.h"

// Global application instances
GLFWwindow* g_Window = nullptr;
AppUI g_AppUI;
CanProtocolHandler g_Protocol;

// Move the WebGL canvas directly to document.body and remove default Emscripten shell clutter.
// Hiding parent divs (.emscripten_border) via CSS would also hide the child #canvas!
EM_JS(void, js_setup_fullscreen_canvas, (), {
    const canvas = document.getElementById('canvas') || document.querySelector('canvas');
    if (canvas) {
        // Move canvas out of .emscripten_border directly into body so it is never hidden by parent containers
        if (canvas.parentElement !== document.body) {
            document.body.appendChild(canvas);
        }
        // Hide all other sibling elements in body (spinner, logo, status text, textarea)
        Array.from(document.body.children).forEach(el => {
            if (el !== canvas && el.tagName !== 'SCRIPT') {
                el.style.display = 'none';
            }
        });
        Module.canvas = canvas;
    }

    const style = document.createElement('style');
    style.textContent =
        "html, body { margin: 0; padding: 0; width: 100%; height: 100%; overflow: hidden; background-color: #1a1a1f; }" +
        "canvas { position: fixed; top: 0; left: 0; width: 100vw; height: 100vh; display: block; border: none; outline: none; }";
    document.head.appendChild(style);
});

// Query current browser viewport width in CSS pixels
EM_JS(int, js_get_window_width, (), {
    return window.innerWidth || document.documentElement.clientWidth || 1280;
});

// Query current browser viewport height in CSS pixels
EM_JS(int, js_get_window_height, (), {
    return window.innerHeight || document.documentElement.clientHeight || 800;
});

// Main non-blocking Emscripten frame loop
void MainLoopStep() {
    glfwPollEvents();

    // 1. Keep GLFW window size and canvas buffer strictly synchronized with browser viewport
    int browser_w = js_get_window_width();
    int browser_h = js_get_window_height();
    int win_w = 0, win_h = 0;
    glfwGetWindowSize(g_Window, &win_w, &win_h);

    if (browser_w > 0 && browser_h > 0 && (browser_w != win_w || browser_h != win_h)) {
        glfwSetWindowSize(g_Window, browser_w, browser_h);
    }

    // 2. Start new Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();

    // Guarantee valid DisplaySize even on the very first frame
    ImGuiIO& io = ImGui::GetIO();
    if (browser_w > 0 && browser_h > 0) {
        io.DisplaySize = ImVec2(static_cast<float>(browser_w), static_cast<float>(browser_h));
    }

    ImGui::NewFrame();

    // 3. Render all PCAN-View workspace windows
    g_AppUI.Render();

    // 4. Render OpenGL draw data
    ImGui::Render();

    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(g_Window, &fb_w, &fb_h);
    if (fb_w <= 0) fb_w = browser_w;
    if (fb_h <= 0) fb_h = browser_h;

    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.12f, 0.12f, 0.15f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(g_Window);
}

int main(int, char**) {
    // 1. Detach canvas from Emscripten wrapper divs and make it fullscreen
    js_setup_fullscreen_canvas();

    // 2. Initialize GLFW
    if (!glfwInit()) {
        std::printf("[ERR] Failed to initialize GLFW\n");
        return -1;
    }

    // Configure OpenGL ES 3.0 (WebGL 2.0) context hints
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    int initial_w = js_get_window_width();
    int initial_h = js_get_window_height();

    g_Window = glfwCreateWindow(initial_w, initial_h, "PCAN-View WebUSB Edition", nullptr, nullptr);
    if (!g_Window) {
        std::printf("[ERR] Failed to create GLFW window\n");
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(g_Window);

    // 3. Initialize Dear ImGui context and backends
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Disable saving imgui.ini to virtual filesystem to prevent layout glitches
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(g_Window, true);
#if IMGUI_VERSION_NUM >= 19100
    // Required in newer Dear ImGui (1.91+ / 1.92+) for proper Emscripten wheel/canvas event binding
    ImGui_ImplGlfw_InstallEmscriptenCallbacks(g_Window, "#canvas");
#endif
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // 4. Connect WebUSB transport callbacks to UI and CAN protocol handlers
    auto& webusb = WebUsbBridge::GetInstance();

    webusb.SetOnConnected([](const UsbDeviceInfo& info) {
        g_AppUI.OnDeviceConnected(info.manufacturer + " " + info.product);
    });

    webusb.SetOnDisconnected([]() {
        g_AppUI.OnDeviceDisconnected();
    });

    webusb.SetOnControlIn([](uint8_t request, const uint8_t* data, int length) {
        g_AppUI.OnControlDataReceived(request, data, length);
    });

    webusb.SetOnControlOutDone([](uint8_t request, bool ok) {
        g_AppUI.OnControlOutDone(request, ok);
    });

    webusb.SetOnDataReceived([](int ep, const uint8_t* data, int length, double host_time_ms) {
        if (ep == 0x81) {
            auto messages = g_Protocol.ParseRxBuffer(data, length);
            for (auto& msg : messages) {
                msg.host_time_ms = host_time_ms;
                g_AppUI.OnCanMessageReceived(msg);
            }
        }
    });

    webusb.Initialize();

    std::printf("[SYS] Starting main render loop (%dx%d)...\n", initial_w, initial_h);

    // 5. Start the browser rendering loop.
    // Do NOT put cleanup code after this call in Emscripten builds.
    emscripten_set_main_loop(MainLoopStep, 0, true);

    return 0;
}