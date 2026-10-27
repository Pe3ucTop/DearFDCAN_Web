# DearFDCAN_Web

FDCAN viewer for the web, in the spirit of PCAN-View: Dear ImGui user interface, WebUSB transport and the
GS-USB (candleLight / CANable style) protocol, compiled to WebAssembly with Emscripten. It runs in a
Chromium-based browser (WebUSB is not available in Firefox/Safari) and talks to the adapter directly, no
driver or native application needed.

## Features

- Classic CAN and CAN FD (BRS, ESI), standard and extended IDs, remote frames.
- **Receive Table by ID**: one row per ID with count, cycle time (min/max kept internally), changed bytes highlighted, sortable columns.
- **Receive Trace**: chronological list with filter by ID, pause, auto-scroll.
- **Transmit**: composer, burst, periodic jobs with optional auto-increment byte, history of sent frames.
- Bit timing: automatic BRP/segment calculation from the adapter clock for a target bit rate and sample point (nominal and data phase), manual override.
- Cycle times and trace timestamps use the adapter's hardware timestamps when the firmware supports them.
- Initial bit rate / sample point can be passed in the URL (see below).
- Whole application can be shipped as one self-contained `index.html`.

## Architecture

```
 page (index.html)                         dedicated Worker (usb_worker.js)
 ┌──────────────────────────┐   postMessage   ┌──────────────────────────────────────────┐
 │ Dear ImGui UI (wasm)     │ ──commands────▶ │ owns the USBDevice                       │
 │ app_ui / main            │                 │  • 6 concurrent transferIn slots         │
 │ webusb_bridge (EM_JS)    │ ◀──snapshots─── │  • ordered control queue, bounded TX     │
 │ id table / trace views   │   (transfer,    │  • watchdog, recovery, unplug handling   │
 └──────────────────────────┘    no copy)     │ can_core (second wasm module)            │
                                              │  • parses gs_usb frames                  │
                                              │  • ID table, trace queue, cycle times    │
                                              │  • adapter→page clock mapping            │
                                              └──────────────────────────────────────────┘
```

- The page never sees individual frames of a busy bus. The worker core aggregates them and publishes a
  *snapshot* about every 16 ms: the changed ID rows (latest state, so updates of one ID coalesce) and the new trace entries.
- Flow control is credit based: at most 4 snapshots may be unacknowledged. If the page is slow the core keeps
  aggregating (bounded memory) instead of queueing frames.
- No `SharedArrayBuffer`, `Atomics` or pthreads: only `postMessage` with transferred `ArrayBuffer`s, so no COOP/COEP headers are needed.
- `requestDevice()` is window-only, so the page asks for the permission and the worker opens the permitted device by VID/PID/serial.

## Requirements

| What | Tested with |
|---|---|
| Emscripten SDK | 6.0.10 (`emsdk-exec.sh` sources `/opt/Emscripten_SDK/emsdk_env.sh`, adjust the path for your machine) |
| Dear ImGui | 1.93.0, sources in `IMGUI_DIR` (default `/Src_Libs/imgui`, see `Makefile`) |
| GNU make | 4.3 |
| Python 3 | 3.12 (standard library only, used by `pack_worker.py`) |
| Browser | Chromium-based (Chrome/Edge) with WebUSB, served over `https://` or `http://localhost` |
| Adapter | GS-USB compatible firmware (candleLight FD style); hardware timestamps need the `HW_TIMESTAMP` feature |

On Linux the adapter usually needs a udev rule that grants your user access to the USB device.

## Build

```sh
make -j                        # single (default): build/single/index.html, one self-contained file
make -j MODE=multi             # build/multi/: index.html index.js index.wasm usb_worker.js can_core.js can_core.wasm
make serve                     # serve the selected mode on http://localhost:8000/
make clean                     # remove build/
make help                      # all variables
```

`./compile_script.sh [single|multi]` is a thin wrapper for the same (`make -j MODE=...`). Both modes can coexist.

`single` builds the CAN core with `-sSINGLE_FILE`, packs its glue together with `usb_worker.js` into a string
(`pack_worker.py`) and embeds that into the page, which starts the worker from a Blob URL. Deploy `index.html` alone.
`multi` keeps the worker as a separate file; deploy all six files from one directory.

Layout of the build directory: `build/<mode>/` holds the results, `build/obj/` the compiled objects (shared by both modes, so
Dear ImGui is compiled only once and a changed source recompiles just itself), `build/tmp/<mode>/` intermediates.
The page and the core are built with `-O2`; override with `make OPT="-O0 -g"` for debugging. `IMGUI_DIR`, `BUILD` and `EMSDK_EXEC` can be set the same way.

Serve the result over HTTPS or `http://localhost` (WebUSB needs a secure context), for example with `make serve`
(or `python3 -m http.server 8000` inside the output directory).

Do not rely on `file://`. If the page has a Content-Security-Policy, it must allow `worker-src blob:` for the single-file build.

## Usage

1. Open the page and press **Request WebUSB CAN Device**, pick the adapter.
2. Adjust bit timing in **Hardware & Bit-Timing Setup** (FD, loopback and listen-only switches are there too) and press **Start CAN Channel**.
   The channel counts as open only after the adapter acknowledged the mode request.
3. Watch the **Receive Table** and **Trace**, send frames from the **Transmit** window.

The menu bar shows live transport counters: USB transfers per second, parsed frames, dropped trace entries,
missing snapshots, dropped TX frames and watchdog recoveries.

### URL parameters

Bit rate and sample point can be preset in the query string; they are used as the targets of the automatic timing calculation.

```
index.html?baudrate=500k&sample_point=87.5&data_baudrate=2M&data_sample_point=75
```

| Parameter | Meaning | Accepted forms |
|---|---|---|
| `baudrate` | nominal bit rate | `500k`, `500kbps`, `2M`, `500` (kbit/s if < 10000), `500000` (bit/s); 10 kbit/s .. 12 Mbit/s |
| `sample_point` | nominal sample point | `87.5`, `87.5%`, `0.875`; 50 .. 95 % |
| `data_baudrate` | CAN FD data phase bit rate | as `baudrate` |
| `data_sample_point` | CAN FD data phase sample point | as `sample_point` |

Invalid values are ignored (the default stays) and reported in the browser console as `[URL] ignored invalid ...`.
Defaults: 500 kbit/s at 87.5 % nominal, 2000 kbit/s at 75 % data. The bit rate is applied in whole kbit/s.

## Source layout

All sources and headers are in `src_inc/`:

| File | Role |
|---|---|
| `src_inc/main.cpp` | GLFW/ImGui setup, main loop, wiring of bridge callbacks |
| `src_inc/app_ui.cpp/.h` | All windows, channel start/stop, snapshot ingestion, URL parameters |
| `src_inc/url_params.h` | Parsing of the URL parameter values |
| `src_inc/webusb_bridge.cpp/.h` | Page side of the worker link (EM_JS), snapshot validation, transport statistics |
| `src_inc/usb_worker.js` | Worker: WebUSB I/O, RX slots, control/TX queues, watchdog, snapshot publishing |
| `src_inc/can_core.cpp` | Worker wasm module: frame parsing, ID table, trace queue, clock mapping |
| `src_inc/can_protocol.cpp/.h` | gs_usb structures, DLC helpers, bit-timing calculation, `CanMessage`/`CanPayload` |
| `src_inc/can_wire.h` | Snapshot format shared by the page and the worker core |

Build tooling in the repository root:

| File | Role |
|---|---|
| `Makefile` | Build driver (modes, object cache, `serve`, `clean`) |
| `compile_script.sh` | Wrapper around `make` for `single` / `multi` |
| `emsdk-exec.sh` | Sets up the Emscripten environment and runs a command (edit the SDK path for your machine) |
| `pack_worker.py` | Packs the worker for the single-file build |

Tunables: `RX_SLOTS`, `TX_INFLIGHT`, `PUBLISH_MS`, `BATCH_CREDITS` at the top of `src_inc/usb_worker.js`; table/trace limits at the top of `src_inc/can_core.cpp`.

## Known limitations

- Chromium only. The adapter is matched by VID/PID/serial; two identical adapters without serial numbers are ambiguous.
- `TX_INFLIGHT = 4` keeps several bulk OUT transfers in flight; WebUSB does not guarantee their order. Set it to `1` if strict frame order matters.
- The adapter→page clock mapping is an estimate (lowest observed latency, 100 ppm drift allowance); cycle times use the raw adapter timestamps and are exact.
  Without hardware timestamps cycle times fall back to arrival times and show USB/browser jitter.
- The ID table holds at most 65536 IDs, the page keeps 2000 trace rows.
- The page wasm has Emscripten's default fixed 16 MB heap (no `ALLOW_MEMORY_GROWTH`); the ID table costs roughly 250 bytes per ID.
- There is no automated test suite in the repository yet.

## Change history

What the code looked like before the rework and what was actually changed, written from the code itself
(the git history was created afterwards and does not follow these steps).

**Before (2026-10-20, starting point).** The page owned the USB device. Reception was a single `transferIn` that
was re-issued only after the previous one completed, so between two requests no receive buffer was posted. Every transfer
cost a `malloc`/`free`, a copy, a `std::function` call and a `std::vector` allocation, and only the first frame
of a transfer was parsed. Frame times were taken on the main thread in the callback, i.e. after rendering
delays. Starting and stopping the channel quickly could leave two receive chains running; any receive error
stopped reception for good; control requests were fired without waiting for each other and the channel was shown
as open before the adapter had acknowledged the mode request; TX had no backpressure and every frame used echo id 0; a
cable unplug was not detected.

**2026-10-20, USB pump and races** (`usb_worker.js`, `webusb_bridge.*`, `app_ui.cpp`)
- 6 concurrent `transferIn` slots, each re-armed first thing in its completion handler.
- Generation token instead of a start/stop flag: stale completions are ignored; the pump lives for the whole connection (WebUSB cannot cancel a pending transfer), "bus off/on" only gates processing.
- Recovery after repeated errors by `releaseInterface`/`claimInterface`; `clearHalt` on STALL; watchdog that probes the control pipe (a silent bus is not a failure); `disconnect` event handling.
- Control requests run in a strict queue with a 2 s timeout each and an acknowledgement; the channel opens only after the `MODE_START` ack, and a failed start sequence is rolled back (a rejected `HOST_FORMAT` is tolerated).
- Bounded TX queue (4096) with at most 4 transfers in flight; unique `echo_id` per frame.
- A transfer holding several equally sized frames is split into frames when the stride is unambiguous.

**2026-10-20, Worker.** All USB I/O moved from the page into a dedicated Worker (WebUSB is available there, `requestDevice()`
stays on the page). Page and worker exchange `postMessage` with transferred buffers and credit-based flow control; no shared memory.

**2026-10-20, CAN core in the worker** (`can_core.cpp`, `can_wire.h`). A second wasm module inside the worker parses frames,
keeps the ID table and the trace queue and publishes snapshots, so the page handles about 60 small updates per second regardless of
bus load. `CanMessage` became plain data (`CanPayload` = fixed 64-byte array instead of `std::vector`) so frames and table rows
move between the two wasm modules with `memcpy`. "Reset Table"/"Clear Trace" commands are acknowledged by sequence number so
stale snapshots cannot resurrect cleared rows.

**2026-10-20, hardware timestamps.** The adapter's `HW_TIMESTAMP` mode flag is now enabled when the adapter reports the
feature (before, no timestamp was ever requested). Cycle time per ID is the difference of 32-bit adapter timestamps (wrap-safe).
Trace times come from a mapping of the adapter clock onto the page clock.

**2026-10-20, single-file build.** `-sSINGLE_FILE` for the page and the core, plus `pack_worker.py` and a Blob-URL worker, so one
`index.html` is enough (confirmed working with WebUSB on real hardware). `compile_script.sh` got `single`/`multi` modes, `set -e`,
a fixed ImGui include path (`./$IMGUI_DIR` with an absolute directory pointed to a non-existent folder) and no longer relies on `source`
under `/bin/sh` (dash); builds go through `emsdk-exec.sh`.

**2026-10-23, URL parameters.** `baudrate`, `sample_point`, `data_baudrate`, `data_sample_point` (`url_params.h`, `AppUI::ApplyUrlParameters`).

**2026-10-27, repository cleanup.** Build outputs are ignored, the unused `CAN_ERR_FLAG` constant and the commented-out SDL/pthread
experiments in the build script were removed, `compile_script.sh` usage text matches its name, this README was written.

**2026-10-27, build layout.** Sources, headers and the worker moved to `src_inc/`. The build logic moved from the shell script into a
`Makefile`: results go to `build/<mode>/`, compiled objects are cached in `build/obj` (Dear ImGui is compiled once; a change recompiles only the
affected files, tracked through generated dependency files), `compile_script.sh` is a wrapper. The page is now compiled and linked with `-O2`
(it used no optimization flags before); the single-file `index.html` shrank from about 1.7 MB to 0.87 MB. The `-O2` page could not be run in a
browser in the environment it was built in; it was only checked statically (the optimized JS parses, the Blob-worker start, the snapshot call and the URL
reader are present, every identifier used by the `EM_JS` bodies is still defined).

What was and was not verified: the CAN core, frame parser and URL parsing were unit-tested natively (ASan/UBSan) and the worker was exercised
against a mock `USBDevice` with the real core module in Node; the unoptimized single-file Blob-Worker was confirmed on hardware. Throughput numbers
and the accuracy of hardware timestamps were not measured.

## License

MIT, see `LICENSE`.
