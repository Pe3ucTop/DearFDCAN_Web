#include <iostream>
#include <libusb-1.0/libusb.h>
#include <emscripten.h>

// Helper function to process libusb transfers
void read_from_device(libusb_device_handle *handle, unsigned char endpoint) {
    unsigned char buffer[64];
    int transferred = 0;

    // Under the hood, Emscripten's Asyncify yields execution to the browser's
    // event loop while WebUSB resolves the underlying asynchronous transfer.
    int result = libusb_bulk_transfer(
        handle,
        endpoint,
        buffer,
        sizeof(buffer),
        &transferred,
        5000 // Timeout in milliseconds
    );

    if (result == LIBUSB_SUCCESS) {
        std::cout << "Successfully read " << transferred << " bytes." << std::endl;
    } else {
        std::cerr << "Transfer failed with error: " << libusb_error_name(result) << std::endl;
    }
}

int main() {
    // Initialize libusb context
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) < 0) {
        std::cerr << "Failed to initialize libusb" << std::endl;
        return 1;
    }

    std::cout << "libusb initialized successfully for WebUSB." << std::endl;

    // Note: In WebUSB, requesting a device list requires user interaction
    // (a Browser UI prompt triggered by a user gesture).
    libusb_device **devs;
    ssize_t count = libusb_get_device_list(ctx, &devs);
    if (count < 0) {
        std::cerr << "Failed to get device list" << std::endl;
        libusb_exit(ctx);
        return 1;
    }

    std::cout << "Found " << count << " USB device(s)." << std::endl;

    // Free the device list when no longer needed
    libusb_free_device_list(devs, 1);
    libusb_exit(ctx);

    return 0;
}