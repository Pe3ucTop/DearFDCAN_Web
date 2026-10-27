#include <SDL.h>
#include <SDL_opengles2.h>
#include <emscripten.h>
#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

#include "webusb_bridge.h"
#include "can_protocol.h"
#include "app_ui.h"

// Global instances
SDL_Window* g_Window = nullptr;
SDL_GLContext g_GLContext = nullptr;
AppUI g_AppUI;
CanProtocolHandler g_Protocol;

// Main Emscripten loop iteration
void MainLoopStep() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Draw the application interface
    g_AppUI.Render();

    ImGui::Render();
    int w, h;
    SDL_GetWindowSize(g_Window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(g_Window);
}

int main(int, char**) {
    // Initialize SDL and OpenGL ES 3.0 context for WebGL2
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    g_Window = SDL_CreateWindow("Modular WebUSB CAN", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    g_GLContext = SDL_GL_CreateContext(g_Window);
    SDL_GL_MakeCurrent(g_Window, g_GLContext);

    // Setup Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(g_Window, g_GLContext);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Connect WebUSB bridge callbacks to the UI and Protocol layers
    auto& webusb = WebUsbBridge::GetInstance();
    
    webusb.SetOnConnected([](const UsbDeviceInfo& info) {
        g_AppUI.OnDeviceConnected(info.manufacturer + " " + info.product);
    });
    
    webusb.SetOnDisconnected([]() {
        g_AppUI.OnDeviceDisconnected();
    });
    
    webusb.SetOnDataReceived([](int ep, const uint8_t* data, int length) {
        // Only process data from the Bulk IN endpoint (usually 0x81)
        if (ep == 0x81) {
            auto messages = g_Protocol.ParseRxBuffer(data, length);
            for (const auto& msg : messages) {
                g_AppUI.OnCanMessageReceived(msg);
            }
        }
    });

    webusb.Initialize();

    // Start the browser rendering loop
    emscripten_set_main_loop(MainLoopStep, 0, true);

    return 0;
}