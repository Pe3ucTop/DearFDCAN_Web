// Save as pre.js and pass "--pre-js pre.js" to emcc
// Fallback polyfill for single-threaded builds where HEAP32 is backed by a regular ArrayBuffer
(function() {
    const nativeWaitAsync = Atomics.waitAsync;
    Atomics.waitAsync = function(ia, index, value, timeout) {
        // Check if the underlying buffer is an instance of SharedArrayBuffer
        if (typeof SharedArrayBuffer !== 'undefined' && ia.buffer instanceof SharedArrayBuffer) {
            return nativeWaitAsync(ia, index, value, timeout);
        }

        // Single-threaded ArrayBuffer fallback:
        // If the value in memory already differs from expected, return immediately
        if (ia[index] !== value) {
            return { async: false, value: 'not-equal' };
        }

        // If timeout is 0 (like in non-blocking libusb_handle_events_timeout_completed),
        // avoid creating a Promise so Asyncify doesn't even unwind the stack unnecessarily
        if (timeout === 0) {
            return { async: false, value: 'timed-out' };
        }

        // Otherwise, yield to the browser event loop via setTimeout so WebUSB Promises can resolve
        const delay = (timeout === undefined || timeout === Infinity) ? 10 : timeout;
        return {
            async: true,
            value: new Promise((resolve) => {
                setTimeout(() => {
                    resolve('timed-out');
                }, delay);
            })
        };
    };
})();