#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>

#include <emscripten.h>
#include <SDL.h>
#include <SDL_opengles2.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

#include <libusb-1.0/libusb.h>

// Global application state
struct AppState {
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;

    libusb_context* usb_ctx = nullptr;
    libusb_device_handle* dev_handle = nullptr;
    bool is_connected = false;

    // Configurable USB parameters
    int interface_num = 0;
    int ep_out = 0x01; // Bulk OUT endpoint address
    int ep_in = 0x81;  // Bulk IN endpoint address

    // Buffers and UI logs
    char send_payload[256] = "Hello WebUSB!";
    std::vector<std::string> log_messages;
    unsigned char rx_buffer[512];

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

// Callback invoked when an asynchronous IN transfer completes
void LIBUSB_CALL on_in_transfer_complete(struct libusb_transfer* transfer) {
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        // Format received bytes as both HEX and ASCII
        std::string hex_output;
        std::string ascii_output;
        char byte_str[8];

        for (int i = 0; i < transfer->actual_length; ++i) {
            unsigned char b = transfer->buffer[i];
            snprintf(byte_str, sizeof(byte_str), "%02X ", b);
            hex_output += byte_str;
            ascii_output += (b >= 32 && b <= 126) ? (char)b : '.';
        }

        g_App.AddLog("[RX <- EP 0x%02X] (%d bytes) HEX: %s | ASCII: %s",
                     transfer->endpoint, transfer->actual_length,
                     hex_output.c_str(), ascii_output.c_str());
    } else {
        g_App.AddLog("[RX <- EP 0x%02X] Failed or Timed Out (Status: %s)",
                     transfer->endpoint, libusb_error_name(transfer->status));
    }

    // Free transfer structure after processing
    libusb_free_transfer(transfer);
}

// Callback invoked when an asynchronous OUT transfer completes
void LIBUSB_CALL on_out_transfer_complete(struct libusb_transfer* transfer) {
    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        g_App.AddLog("[TX -> EP 0x%02X] Sent %d bytes successfully.",
                     transfer->endpoint, transfer->actual_length);
    } else {
        g_App.AddLog("[TX -> EP 0x%02X] Send Error (Status: %s)",
                     transfer->endpoint, libusb_error_name(transfer->status));
    }

    // Free dynamically allocated buffer and the transfer structure
    delete[] transfer->buffer;
    libusb_free_transfer(transfer);
}

// Request device permission from browser and open the selected USB device
void request_and_connect_usb() {
    if (g_App.dev_handle) {
        g_App.AddLog("[WARN] Device is already connected.");
        return;
    }

    libusb_device** devs = nullptr;

    // In Emscripten's WebUSB backend, libusb_get_device_list triggers
    // navigator.usb.requestDevice() if called from a user gesture context.
    ssize_t cnt = libusb_get_device_list(g_App.usb_ctx, &devs);
    if (cnt < 0) {
        g_App.AddLog("[ERR] libusb_get_device_list failed: %s", libusb_error_name((int)cnt));
        return;
    }

    if (cnt == 0) {
        g_App.AddLog("[INFO] No WebUSB device selected by user.");
        libusb_free_device_list(devs, 1);
        return;
    }

    // Open the first granted device
    int res = libusb_open(devs[0], &g_App.dev_handle);
    if (res == LIBUSB_SUCCESS && g_App.dev_handle != nullptr) {
        libusb_device_descriptor desc;
        libusb_get_device_descriptor(devs[0], &desc);
        g_App.AddLog("[USB] Connected to VID: 0x%04X, PID: 0x%04X", desc.idVendor, desc.idProduct);

        // Claim the target interface
        res = libusb_claim_interface(g_App.dev_handle, g_App.interface_num);
        if (res == LIBUSB_SUCCESS) {
            g_App.AddLog("[USB] Claimed interface %d.", g_App.interface_num);
            g_App.is_connected = true;
        } else {
            g_App.AddLog("[WARN] Could not claim interface %d: %s",
                         g_App.interface_num, libusb_error_name(res));
            // Keep handle open even if claim fails so user can test other interfaces
            g_App.is_connected = true;
        }
    } else {
        g_App.AddLog("[ERR] Failed to open device: %s", libusb_error_name(res));
    }

    libusb_free_device_list(devs, 1);
}

// Submit an asynchronous Bulk OUT transfer
void submit_out_packet() {
    if (!g_App.is_connected || !g_App.dev_handle) return;

    size_t len = strlen(g_App.send_payload);
    if (len == 0) return;

    // Allocate buffer that persists until the completion callback fires
    unsigned char* out_buf = new unsigned char[len];
    memcpy(out_buf, g_App.send_payload, len);

    struct libusb_transfer* transfer = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(
        transfer,
        g_App.dev_handle,
        (unsigned char)g_App.ep_out,
        out_buf,
        (int)len,
        on_out_transfer_complete,
        nullptr,
        2000 // 2000 ms timeout
    );

    int res = libusb_submit_transfer(transfer);
    if (res < 0) {
        g_App.AddLog("[ERR] Submit OUT transfer failed: %s", libusb_error_name(res));
        delete[] out_buf;
        libusb_free_transfer(transfer);
    }
}

// Submit an asynchronous Bulk IN transfer to read incoming endpoint data
void submit_in_request() {
    if (!g_App.is_connected || !g_App.dev_handle) return;

    struct libusb_transfer* transfer = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(
        transfer,
        g_App.dev_handle,
        (unsigned char)g_App.ep_in,
        g_App.rx_buffer,
        sizeof(g_App.rx_buffer),
        on_in_transfer_complete,
        nullptr,
        2000 // 2000 ms timeout
    );

    int res = libusb_submit_transfer(transfer);
    if (res < 0) {
        g_App.AddLog("[ERR] Submit IN transfer failed: %s", libusb_error_name(res));
        libusb_free_transfer(transfer);
    } else {
        g_App.AddLog("[INFO] Listening for data on Endpoint 0x%02X...", g_App.ep_in);
    }
}

// Disconnect from the current WebUSB device
void disconnect_usb() {
    if (g_App.dev_handle) {
        libusb_release_interface(g_App.dev_handle, g_App.interface_num);
        libusb_close(g_App.dev_handle);
        g_App.dev_handle = nullptr;
        g_App.is_connected = false;
        g_App.AddLog("[USB] Device disconnected.");
    }
}

// Main Emscripten frame loop
void main_loop() {
    // 1. Poll SDL events
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
    }

    // 2. Poll non-blocking libusb events to trigger asynchronous transfer callbacks
    if (g_App.usb_ctx) {
        struct timeval tv = {0, 0}; // Zero timeout for non-blocking poll
        libusb_handle_events_timeout_completed(g_App.usb_ctx, &tv, nullptr);
    }

    // 3. Start Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // 4. Build GUI Windows
    ImGui::SetNextWindowPos(ImVec2(15, 15), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(680, 480), ImGuiCond_FirstUseEver);

    ImGui::Begin("WebUSB + libusb Asynchronous Terminal");

    // --- Connection Bar ---
    if (!g_App.is_connected) {
        ImGui::InputInt("Interface #", &g_App.interface_num);
        if (ImGui::Button("Request WebUSB Device (Browser Prompt)", ImVec2(320, 32))) {
            request_and_connect_usb();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Status: Disconnected");
    } else {
        if (ImGui::Button("Disconnect Device", ImVec2(180, 32))) {
            disconnect_usb();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Status: Connected");
    }

    ImGui::Separator();

    // --- Endpoint Configuration & Packet Controls ---
    ImGui::BeginDisabled(!g_App.is_connected);

    ImGui::Text("Endpoint Configuration (Hex):");
    ImGui::PushItemWidth(120);
    ImGui::InputInt("Bulk OUT EP", &g_App.ep_out, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::InputInt("Bulk IN EP", &g_App.ep_in, 1, 16, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::InputText("Payload (ASCII)", g_App.send_payload, sizeof(g_App.send_payload));

    if (ImGui::Button("Send Packet (Async OUT)", ImVec2(200, 30))) {
        submit_out_packet();
    }
    ImGui::SameLine();
    if (ImGui::Button("Read Endpoint (Async IN)", ImVec2(200, 30))) {
        submit_in_request();
    }

    ImGui::EndDisabled();
    ImGui::Separator();

    // --- Endpoint Data & Event Log Window ---
    ImGui::Text("Received Endpoint Data & Transfer Logs:");
    ImGui::SameLine(ImGui::GetWindowWidth() - 100);
    if (ImGui::SmallButton("Clear Log")) {
        g_App.log_messages.clear();
    }

    ImGui::BeginChild("EndpointDataLog", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& entry : g_App.log_messages) {
        ImGui::TextUnformatted(entry.c_str());
    }
    // Auto-scroll to the bottom when new data arrives
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();

    // 5. Render OpenGL frame
    ImGui::Render();
    ImGuiIO& io = ImGui::GetIO();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(g_App.window);
}

int main(int, char**) {
    // Initialize SDL2 video subsystem
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        printf("SDL_Init Error: %s\n", SDL_GetError());
        return -1;
    }

    // Configure WebGL2 / OpenGL ES 3.0 context
    const char* glsl_version = "#version 300 es";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    g_App.window = SDL_CreateWindow(
        "ImGui WebUSB Client",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        720, 520,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );
    g_App.gl_context = SDL_GL_CreateContext(g_App.window);
    SDL_GL_MakeCurrent(g_App.window, g_App.gl_context);

    // Initialize Dear ImGui context and backends
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(g_App.window, g_App.gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Initialize libusb context
    int res = libusb_init(&g_App.usb_ctx);
    if (res < 0) {
        g_App.AddLog("[ERR] Failed to init libusb: %s", libusb_error_name(res));
    } else {
        g_App.AddLog("[SYS] libusb WebUSB backend initialized. Ready.");
    }

    // Start Emscripten non-blocking browser render loop
    emscripten_set_main_loop(main_loop, 0, true);

    // Cleanup resources on exit
    disconnect_usb();
    if (g_App.usb_ctx) {
        libusb_exit(g_App.usb_ctx);
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(g_App.gl_context);
    SDL_DestroyWindow(g_App.window);
    SDL_Quit();

    return 0;
}