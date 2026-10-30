# DearFDCAN_Web

**▶ Run it online: <https://pe3uctop.github.io/DearFDCAN_Web/>** (Chrome/Edge, adapter plugged in, nothing to install)

FDCAN viewer for the web, in the spirit of PCAN-View: Dear ImGui user interface, WebUSB transport and the
GS-USB (candleLight / CANable style) and PEAK PCAN-USB FD protocols over WebUSB, plus SLCAN (Lawicel / CANable slcan firmware)
over Web Serial, compiled to WebAssembly with Emscripten. It runs in a Chromium-based browser (WebUSB and Web Serial are not
available in Firefox/Safari) and talks to the adapter directly, no driver or native application needed.

## Features

- Classic CAN and CAN FD (BRS, ESI), standard and extended IDs, remote frames.
- **Two adapter families**: gs_usb (candleLight / CANable and compatible) and PEAK PCAN-USB FD (VID 0C72, PID 0011..0014, one CAN port);
  the protocol is chosen from the USB ids and adapters of both families can be used side by side.
- **ElmueSoft CANable 2.5 extensions of gs_usb** (used automatically when the firmware offers them, can be switched off per adapter): compact
  messages instead of 80 byte frames, several frames per USB transfer ("blobs"), tx echoes by marker, error reports (bus state, error counters,
  decoded error flags, shown in the trace and in the device pane), bus load in percent, text messages of the firmware, board information,
  the real bit-timing limits of the data phase, the 120 Ω termination switch and up to eight hardware ID filters in the adapter.
- **PEAK PCAN-USB FD reports**: error and status records (bus state, error counters, error type, shown like the Elmue reports), bus load, and
  the adapter's calibration records (its clock, sent about once a second), from which the drift and the spread of the adapter's clock are shown.
- **Clock synchronisation** of gs_usb adapters with hardware timestamps: once a second the adapter's clock is read (`GS_USB_BREQ_TIMESTAMP`),
  the round trip of each request brackets the moment it was latched, and a line fitted through the samples with the shortest round trips gives
  the offset and the drift of the adapter's quartz against the browser clock. The Devices window shows the drift in ppm and the **spread** of the
  samples around the line (rms and worst case in µs), the accuracy the frame times can have.
- **Serial SLCAN adapters** through Web Serial (**Add Serial Port**): the CANable 2.5 firmware dialect (CAN FD, exact bit timing from the
  adapter's own limits, listen-only and loopback modes) and classic Lawicel adapters (fixed bit rates, no FD); the dialect is detected with
  the `V` command or chosen by hand. USB and serial adapters can be mixed.
- **Several adapters at once** (up to 8). Every adapter gets a device number (1..8) and its own bit timing, mode and statistics;
  frames of all adapters are received simultaneously.
- **Devices window**: add / connect / disconnect / remove adapters, state of each one (Connecting, Connected, BUS ON, Disconnected),
  start and stop the CAN channel per adapter or for all, and an information pane with adapter name, manufacturer, serial number,
  VID:PID, USB and device revision, firmware / hardware version, CAN channel count, CAN clock, feature flags, endpoints and USB counters.
- **Receive Table by ID**: one row per device and ID with count, cycle time (min/max kept internally), changed bytes highlighted,
  sortable columns, `Dev` column and a device filter.
- **Receive Trace**: chronological list with `Dev` column, filter by ID and by device, pause, auto-scroll.
- **Transmit**: composer, burst, periodic jobs with optional auto-increment byte, history of sent frames; the target is one
  adapter or all adapters that are BUS ON, and jobs and history entries remember it.
- Bit timing: automatic BRP/segment calculation from the adapter clock for a target bit rate and sample point (nominal and data phase), manual override.
- Cycle times and trace timestamps use the adapter's hardware timestamps when the firmware supports them.
- Initial bit rate / sample point can be passed in the URL (see below).
- Whole application can be shipped as one self-contained `index.html`.

## Architecture

```
 page (index.html)                         dedicated Worker (usb_worker.js)
 ┌──────────────────────────┐   postMessage   ┌──────────────────────────────────────────┐
 │ Dear ImGui UI (wasm)     │ ──commands────▶ │ one Device context per adapter (dev 0-7):│
 │ app_ui / main            │   (dev, epoch)  │  • owns its USBDevice                    │
 │ webusb_bridge (EM_JS)    │ ◀──snapshots─── │  • 6 concurrent transferIn slots         │
 │ device slots, id table,  │   (transfer,    │  • ordered control queue, bounded TX    │
 │ trace views              │    no copy)     │  • watchdog, recovery, unplug handling   │
 └──────────────────────────┘                 │ can_core (one shared wasm module)        │
                                              │  • parses gs_usb frames of all adapters  │
                                              │  • ID table keyed by (device, ID)        │
                                              │  • trace queue, cycle times              │
                                              │  • clock mapping per adapter             │
                                              └──────────────────────────────────────────┘
```

- The page never sees individual frames of a busy bus. The worker core aggregates them and publishes a
  *snapshot* about every 16 ms: the changed ID rows (latest state, so updates of one ID coalesce) and the new trace entries.
- Flow control is credit based: at most 4 snapshots may be unacknowledged. If the page is slow the core keeps
  aggregating (bounded memory) instead of queueing frames.
- Adapters are isolated from each other: every one has its own receive slots, control and TX queues, watchdog and statistics, so a
  hung or unplugged adapter does not delay the others. Only the matching of permitted devices (`getDevices()`) is serialized.
  All commands carry the device number, and every worker message carries the device number and the `epoch` of that connection,
  so messages of an older connection of a slot are discarded.
- **Serial adapters** have a second, I/O-only worker, `serial_worker.js`, started on first use. It owns the `SerialPort` objects (open, read loop,
  ordered writes, DTR/RTS) and nothing else; the page connects it to the CAN worker with a `MessageChannel`, and the bytes it reads travel to
  the CAN worker as transferred `ArrayBuffer`s. The CAN worker (`usb_worker.js`, which therefore does more than USB I/O: it also hosts the core and
  publishes the snapshots) keeps one context per slot for serial adapters too (`SerialDevice`), stages the bytes into the same core, and turns the
  core's reply tokens into command acknowledgements. USB transfers do not take the extra hop. An SLCAN adapter has no hardware timestamps:
  frame times are the arrival times of the serial chunks, spread over the interval since the previous chunk.
- No `SharedArrayBuffer`, `Atomics` or pthreads: only `postMessage` with transferred `ArrayBuffer`s, so no COOP/COEP headers are needed.
  (`MessageChannel` and transferable buffers work over plain `http:` too; WebUSB and Web Serial themselves need `https:` or `localhost`.)
- `requestDevice()` is window-only, so the page asks for the permission (browser chooser) and the worker opens the permitted device
  by VID/PID/serial. The page keeps the picked `USBDevice` in a slot, so a disconnected adapter can be reconnected without the chooser.

## Requirements

| What | Tested with |
|---|---|
| Emscripten SDK | 6.0.10 (`emsdk-exec.sh` sources `/opt/Emscripten_SDK/emsdk_env.sh`, adjust the path for your machine) |
| Dear ImGui | 1.93.0, sources in `IMGUI_DIR` (default `/Src_Libs/imgui`, see `Makefile`) |
| CMake | 3.20 or newer (3.28 used); GNU make 4.3 for the `Makefile` front-end |
| Python 3 | 3.12 (standard library only, used by `pack_worker.py`) |
| Browser | Chromium-based (Chrome/Edge) with WebUSB / Web Serial, served over `https://` or `http://localhost` |
| Adapter | GS-USB compatible firmware (candleLight FD style; hardware timestamps need the `HW_TIMESTAMP` feature), a PEAK PCAN-USB FD family adapter, or an SLCAN adapter on a serial port (the last two are not tested on real hardware, see the change history) |

On Linux the adapter usually needs a udev rule that grants your user access to the USB device. PEAK adapters are claimed by the
`peak_usb` / `pcanfd` kernel driver (Linux) or the PEAK driver (Windows) as soon as they are plugged in; WebUSB can use them only when that
driver is not bound to the device (unbind it, or use WinUSB/Zadig on Windows).

## Build

```sh
make -j                        # single (default): build/single/index.html, one self-contained file, also copied to docs/index.html
make -j MODE=multi             # build/multi/: index.html index.js index.wasm usb_worker.js serial_worker.js webusb_io.js webserial_io.js can_core.js can_core.wasm
make serve                     # serve the selected mode on http://localhost:8000/
make test                      # native tests (CTest, sanitizers) and, if build/multi exists, the worker tests
make clean                     # remove build/
make help                      # all variables
```

The `Makefile` is a front-end for CMake (`emcmake cmake -S . -B build/em`, then `cmake --build build/em --target single`, or `multi`); both
modes come from one build tree, so Dear ImGui is compiled once, and both can coexist. The native tests alone:
`cmake -S . -B build/native && cmake --build build/native && ctest --test-dir build/native`.

`single` builds the CAN core with `-sSINGLE_FILE`, packs its glue together with `webusb_io.js` and `usb_worker.js` into a string
(`pack_worker.py`), packs `webserial_io.js` and `serial_worker.js` into another one and embeds both into the page, which starts the workers
from Blob URLs. Deploy `index.html` alone. `multi` keeps the workers and the two transport libraries as separate files (the workers load the
libraries with `importScripts`); deploy all nine files from one directory.

**GitHub Pages.** The single build is copied to `docs/index.html` (plus an empty `docs/.nojekyll`), the folder GitHub Pages serves.
Build, commit `docs/` and in the repository settings choose *Pages → Deploy from a branch → `main` / `/docs`*; the page is then available
over HTTPS, which WebUSB requires. `make clean` does not touch `docs/` (override the folder with `make DOCS=...`).

Layout of the build directory: `build/<mode>/` holds the results, `build/obj/` the compiled objects (shared by both modes, so
Dear ImGui is compiled only once and a changed source recompiles just itself), `build/tmp/<mode>/` intermediates.
The page and the core are built with `-O2`; override with `make OPT="-O0 -g"` for debugging. `IMGUI_DIR`, `BUILD` and `EMSDK_EXEC` can be set the same way.

Serve the result over HTTPS or `http://localhost` (WebUSB needs a secure context), for example with `make serve`
(or `python3 -m http.server 8000` inside the output directory).

Do not rely on `file://`. If the page has a Content-Security-Policy, it must allow `worker-src blob:` for the single-file build.

## Usage

1. Open the page and press **Add Device** in the **Devices & Bit-Timing Setup** window, pick an adapter in the browser dialog.
   Repeat for every further adapter (up to 8); each gets the next free device number.
2. Select an adapter in the list to see its information and to adjust its bit timing (FD, loopback and listen-only switches are there too),
   then press **Start** in its row (or **Start All**). The channel counts as open only after the adapter acknowledged the mode request.
3. Watch the **Receive Table** and **Trace** (the `Dev` column and the device filter tell the adapters apart; hover a device number to see
   which adapter it is). Pick the target of a transmission with **Send via** in the **Transmit** window.

An adapter that was unplugged or disconnected stays in the list as *Disconnected*; **Connect** reopens it without the browser dialog,
**Remove** forgets it and frees its number. Picking an adapter in the dialog that already has a number reconnects that slot.

The menu bar shows live transport counters: connected adapters and how many are BUS ON, parsed frames, dropped trace entries and
missing snapshots. Per-adapter USB rate, frame count, errors, dropped TX frames and watchdog recoveries are in the information pane.

### URL parameters

Bit rate and sample point can be preset in the query string; they are used as the targets of the automatic timing calculation
and become the defaults of every adapter you add (each adapter can still be changed on its own).

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

The project is split into libraries (`libs/`) and the monitor that uses them (`apps/dearfdcan_web/`); the reasons and the plan are in `DECISIONS.md`.
Layers, from the bottom: `emscripten-webserial` (transport, stand-alone) · `can-core` (types) → `can-protocols` (what the adapters speak) →
`can-engine` (the worker core) and `can-adapters` (one driver per adapter) · the monitor. The CAN libraries have no browser code and are
unit-tested natively.

| File | Role |
|---|---|
| `apps/dearfdcan_web/main.cpp` | GLFW/ImGui setup, main loop, wiring of bridge callbacks |
| `apps/dearfdcan_web/app_ui.cpp/.h` | All windows, adapter slots (`DeviceSlot`: the adapter's state and the user's settings, plus its driver), snapshot ingestion, URL parameters |
| `libs/can-adapters/can_adapter.cpp/.h` | Adapter drivers (gs_usb with Elmue, PEAK, SLCAN) over the `ITransport` interface: capabilities, start / stop sequences and their acknowledgements, bit-timing limits, transmit frames, bus reports; no UI and no Emscripten code |
| `apps/dearfdcan_web/url_params.h` | Parsing of the URL parameter values |
| `apps/dearfdcan_web/webusb_bridge.cpp/.h` | Page side of the worker link (EM_JS): device slots, device events, snapshot validation, per-adapter statistics |
| `apps/dearfdcan_web/usb_worker.js` | CAN worker: one context per adapter (`Device` is the common USB part, a `UsbPort` of `webusb_io.js` connected to the page's messages and to the core; `GsUsbDevice` (clock requests, Elmue echoes) and `PcanDevice` (firmware handshake, calibration statistics) are the protocol classes; `SerialDevice` stages the bytes from `serial_worker.js` and matches command acknowledgements), hosts the core, snapshot publishing |
| `apps/dearfdcan_web/serial_worker.js` | Serial worker: a thin shell around `webserial_io.js` (Web Serial I/O only), linked to the CAN worker by a `MessageChannel` |
| `libs/can-engine/can_core.cpp` | Worker wasm module: frame parsing, ID table keyed by (device, ID), trace queue, per-adapter clock mapping |
| `libs/can-core/can_protocol_common.h` | Protocol independent definitions: `CanMessage`/`CanPayload`, `CanBittiming`, `CanProtocolType`, DLC helpers, bit-timing calculation, `kMaxDevices` |
| `libs/can-protocols/can_protocol_gsusb.cpp/.h` | gs_usb (candleLight / CANable): control requests, structures, bulk frame parsing and building |
| `libs/can-protocols/can_protocol_elmue.cpp/.h` | The ElmueSoft CANable 2.5 extension of gs_usb: message/blob parsing, frame building with echo markers, error decoding, board info, host filters, mode flags |
| `libs/can-protocols/can_protocol_pcan.cpp/.h` | PEAK PCAN-USB FD (uCAN): packed command/record structures, command transfers (clock, options, bit timing, filter, bus on/off), record stream parsing with reassembly across transfers, frame building, bit-timing limits |
| `libs/can-protocols/can_protocol_slcan.cpp/.h` | SLCAN (Lawicel / CANable slcan firmware): line stream parser (frames and command replies), frame lines, `V` answer parser, start/stop command sequences |
| `libs/can-protocols/can_protocol.cpp/.h` | Umbrella header and the `CanProtocolHandler` facade: the single entry point the UI and the core use, forwards to the protocol backend |
| `libs/can-engine/can_wire.h` | Snapshot format shared by the page and the worker core |
| `libs/webusb-io/webusb_io.js` | The WebUSB transport (`UsbPort`): finding and opening a permitted device, receive slots with generations, recovery, ordered control / command transfers, transmit queue, watchdog; no CAN protocol, no wasm, no `postMessage`; see its README |
| `libs/webserial-io/webserial_io.js` | The Web Serial transport (`SerialConnection`): open with DTR/RTS, read loop that survives recoverable stream errors, ordered writes with acknowledgements; see its README |
| `apps/dearfdcan_web/WORKER_PROTOCOL.md` | The messages between the page, the CAN worker and the serial worker |
| `libs/emscripten-webserial/` | Standalone Emscripten library: Web Serial ports as `/dev/ttyWeb0..7` device nodes (POSIX `read`/`write`/`poll`/termios), with its own test; not used by the monitor itself, see its README |

Tests (`tests/`, they need no adapter and no browser):

| File | Role |
|---|---|
| `tests/test_elmue.cpp`, `tests/test_clock_sync.cpp`, `tests/test_pcan_records.cpp` | Unit tests of the protocol layer and the core (Elmue messages, blobs, echoes and a fuzzer; the clock fit against a simulated drifting clock; PEAK records and options), natively under ASan / UBSan |
| `tests/test_app_ui.cpp` | Headless test of the adapter sequences driven by `AppUI` (open, capabilities, start, acknowledgements, stop, transmit, reports) for gs_usb (legacy and Elmue), PEAK and SLCAN, with the real Dear ImGui and a recording stand-in for the bridge, natively under ASan / UBSan |
| `tests/test_webusb_io.js`, `tests/test_webserial_io.js` | The transport libraries against a mock `USBDevice` (matching, receive slots and generations, recovery, loss, ordered control transfers, transmit queue, watchdog) and a mock `SerialPort` on Web Streams (open, signals, reading, ordered writes with acknowledgements, close during open, loss), under Node |
| `tests/test_serial_worker.js` | The real `serial_worker.js` over the library: `pick`, `sopen`, `srx`, `swrite` / `swack`, `sstats`, `sclose`, `forget` (also the packed one) |
| `tests/test_workers.js`, `tests/worker_harness.js` | The real `usb_worker.js` and `can_core.wasm` in a Node `vm` against mock USB adapters (Elmue blobs and echoes, clock requests, PEAK records): `node tests/test_workers.js build/multi apps/dearfdcan_web` after `make MODE=multi`; with `build/tmp/single/usb_worker_bundle.js` as a third argument the packed worker of the single-file build |

`make test` builds and runs the native tests with CTest (in `build/native`) and, when `build/multi` exists, the worker tests. The direction
of the project (libraries for transport and CAN, and the utility) and the decisions taken are in `DECISIONS.md`.

Build tooling in the repository root:

| File | Role |
|---|---|
| `CMakeLists.txt`, `libs/*/CMakeLists.txt`, `apps/dearfdcan_web/CMakeLists.txt`, `tests/CMakeLists.txt` | The description of the build (libraries, the Emscripten page and worker module, the native tests) |
| `Makefile` | Front-end for it with the commands above (`make`, `MODE=multi`, `test`, `serve`, `clean`) |
| `emsdk-exec.sh` | Sets up the Emscripten environment and runs a command (edit the SDK path for your machine) |
| `apps/dearfdcan_web/pack_worker.py` | Packs the workers for the single-file build |

Tunables: `RX_SLOTS`, `TX_INFLIGHT`, `PUBLISH_MS`, `BATCH_CREDITS` at the top of `apps/dearfdcan_web/usb_worker.js`; table/trace limits at the top of `libs/can-engine/can_core.cpp`.

## Known limitations

- Chromium only. An adapter is matched by VID/PID/serial. Identical adapters without serial numbers can be told apart only by
  "not yet in use", so after replugging them their device numbers may be swapped.
- Only CAN channel 0 of an adapter is used; multi-channel adapters (gs_usb with several channels, PCAN-USB X6 / Pro FD with two ports) show their
  channel count but are driven as one channel.
- PEAK specifics: the protocol is picked from the USB ids only (no manual override); every transmitted frame requests a TX echo, there is no
  hardware loopback switch and the CAN clock is fixed at 80 MHz. The calibration records are used for the drift / spread statistics only: PEAK's
  own driver notes that their clock is not guaranteed to share a base with the frame timestamps (the PCAN-USB X6 for one), so the frame times keep
  using the lowest-latency estimate, now following the measured drift instead of a fixed allowance. Cache-critical records and the remaining
  fields of the error codes are not decoded; error type "counter decremented" updates the counters only.
- SLCAN specifics: the protocol cannot be detected from the serial port, so every port added with **Add Serial Port** is SLCAN (dialect detected or
  chosen); a serial port without USB ids is told apart from another one only by its position in the browser's list of permitted ports. There are no
  hardware timestamps (arrival times of the serial chunks, spread inside a chunk) and no TX echo (frames you send do not show up in the trace); a
  classic Lawicel adapter has only the nine standard bit rates (`S0`..`S8`), no FD and no loopback; an error reply to a transmitted frame is only
  counted (TX errors). Whether `SerialPort` objects can be transferred to the serial worker depends on the browser: if not, the worker finds the
  port again with `getPorts()`; whether Web Serial is usable inside the dedicated worker depends on the browser version too.
- At most 8 adapters (`kMaxDevices` in `can_protocol.h`, `MAX_DEVICES` in `usb_worker.js`, `MAX_DEVICES` in `webusb_bridge.cpp`). The limits
  of the ID table (65536 rows) and the trace are shared by all adapters.
- `TX_INFLIGHT = 4` keeps several bulk OUT transfers in flight; WebUSB does not guarantee their order. Set it to `1` if strict frame order matters.
- Elmue protocol specifics: frames are sent one per USB transfer (the adapter can take blobs of several transmit frames, this program does not
  batch them); the bridge filters for multi-channel boards are not exposed; the adapter's flash, pin and error-state requests are not used. The
  receive buffer of gs_usb adapters is 2048 bytes (the size of the largest blob).
- The adapter→page clock mapping is an estimate. gs_usb adapters with timestamps: a line through the clock samples (see Features; needs three
  good samples, until then and for the other protocols: lowest observed latency, 100 ppm drift allowance). Legacy candleLight firmware latches
  the timestamp for this request only at USB start-of-frame (up to 1 ms old), so its spread is larger; the Elmue firmware reads the clock at
  once. Cycle times use the raw adapter timestamps and are exact.
  Without hardware timestamps cycle times fall back to arrival times and show USB/browser jitter.
- The ID table holds at most 65536 IDs, the page keeps 2000 trace rows.
- The page wasm has Emscripten's default fixed 16 MB heap (no `ALLOW_MEMORY_GROWTH`); the ID table costs roughly 250 bytes per ID.
- There is no automated test suite in the repository yet.

## Change history

See [ChangeLog.md](ChangeLog.md).

## License

Apache License 2.0, see `LICENSE`.
