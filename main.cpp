#include <GLFW/glfw3.h>
#include <GLES3/gl3.h>
#include <emscripten.h>
#include <emscripten/html5.h>

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

// Inject CSS rules into the DOM so the WebGL canvas occupies 100% of the browser viewport
// and hides default Emscripten shell decorations (status bar, console, margins)
EM_JS(void, js_setup_fullscreen_canvas_styles, (), {
    const style = document.createElement('style');
    style.innerHTML = `
        html, body {
            margin: 0 !important;
            padding: 0 !important;
            width: 100vw !important;
            height: 100vh !important;
            overflow: hidden !important;
            background-color: #19191f !important;
        }
        #canvas {
            position: fixed !important;
            top: 0 !important;
            left: 0 !important;
            margin: 0 !important;
            padding: 0 !important;
            width: 100vw !important;
            height: 100vh !important;
            border: none !important;
            display: block !important;
        }
        /* Hide standard Emscripten HTML shell elements if default shell is used */
        #status, #controls, #output, .emscripten, .emscripten_border, #emscripten_logo {
            display: none !important;
        }
        /* Make sure canvas itself stays visible even if it has the .emscripten class */
        canvas.emscripten {
            display: block !important;
        }
    `;
    document.head.appendChild(style);
});

// Query current browser inner window dimensions in CSS pixels
EM_JS(int, js_get_window_width, (), {
    return window.innerWidth;
});

EM_JS(int, js_get_window_height, (), {
    return window.innerHeight;
});

// Emscripten browser window resize callback
EM_BOOL OnBrowserWindowResize(int event_type, const EmscriptenUiEvent* ui_event, void* user_data) {
    if (g_Window && ui_event) {
        int new_w = ui_event->windowInnerWidth;
        int new_h = ui_event->windowInnerHeight;
        if (new_w > 0 && new_h > 0) {
            // Resize GLFW window to match the browser viewport
            glfwSetWindowSize(g_Window, new_w, new_h);
        }
    }
    return EM_TRUE;
}

// Main non-blocking Emscripten frame loop
void MainLoopStep() {
    // Poll and process GLFW events (mouse, keyboard, window resize)
    glfwPollEvents();

    // Safeguard: synchronize GLFW window size if browser viewport changed outside resize event
    int browser_w = js_get_window_width();
    int browser_h = js_get_window_height();
    int win_w = 0, win_h = 0;
    glfwGetWindowSize(g_Window, &win_w, &win_h);
    if ((browser_w > 0 && browser_h > 0) && (browser_w != win_w || browser_h != win_h)) {
        glfwSetWindowSize(g_Window, browser_w, browser_h);
    }

    // Start new Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // Render all PCAN-View workspace windows
    g_AppUI.Render();

    // Render OpenGL draw data
    ImGui::Render();
    int fb_w = 0, fb_h = 0;
    glfwGetFramebufferSize(g_Window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.10f, 0.10f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(g_Window);
}

int main(int, char**) {
    // 1. Apply full-viewport CSS rules before creating the WebGL context
    js_setup_fullscreen_canvas_styles();

    // 2. Initialize GLFW library
    if (!glfwInit()) {
        return -1;
    }

    // Configure OpenGL ES 3.0 (WebGL 2.0) context hints
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    int initial_w = js_get_window_width();
    int initial_h = js_get_window_height();
    if (initial_w <= 0) initial_w = 1280;
    if (initial_h <= 0) initial_h = 900;

    g_Window = glfwCreateWindow(initial_w, initial_h, "PCAN-View WebUSB Edition (GLFW)", nullptr, nullptr);
    if (!g_Window) {
        glfwTerminate();
        return -1;
    }

    glfwMakeContextCurrent(g_Window);
    glfwSwapInterval(1); // Enable VSync

    // 3. Register HTML5 window resize callback for dynamic full-window resizing
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_FALSE, OnBrowserWindowResize);

    // 4. Initialize Dear ImGui context and GLFW + OpenGL3 backends
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // Install GLFW callbacks for mouse, scroll, and keyboard input
    ImGui_ImplGlfw_InitForOpenGL(g_Window, true);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // 5. Connect WebUSB transport callbacks to UI and CAN protocol handlers
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

    webusb.SetOnDataReceived([](int ep, const uint8_t* data, int length) {
        if (ep == 0x81) {
            auto messages = g_Protocol.ParseRxBuffer(data, length);
            for (const auto& msg : messages) {
                g_AppUI.OnCanMessageReceived(msg);
            }
        }
    });

    webusb.Initialize();

    // 6. Start the browser rendering loop
    emscripten_set_main_loop(MainLoopStep, 0, true);

    // Cleanup on exit
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(g_Window);
    glfwTerminate();

    return 0;
}