# DearFDCAN_Web

FDCAN viewer for the web, in the spirit of PCAN-View: Dear ImGui user interface, WebUSB transport and the
GS-USB (candleLight / CANable style) and PEAK PCAN-USB FD protocols, compiled to WebAssembly with Emscripten. It runs in a
Chromium-based browser (WebUSB is not available in Firefox/Safari) and talks to the adapter directly, no
driver or native application needed.

## Features

- Classic CAN and CAN FD (BRS, ESI), standard and extended IDs, remote frames.
- **Two adapter families**: gs_usb (candleLight / CANable and compatible) and PEAK PCAN-USB FD (VID 0C72, PID 0011..0014, one CAN port);
  the protocol is chosen from the USB ids and adapters of both families can be used side by side.
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
- No `SharedArrayBuffer`, `Atomics` or pthreads: only `postMessage` with transferred `ArrayBuffer`s, so no COOP/COEP headers are needed.
- `requestDevice()` is window-only, so the page asks for the permission (browser chooser) and the worker opens the permitted device
  by VID/PID/serial. The page keeps the picked `USBDevice` in a slot, so a disconnected adapter can be reconnected without the chooser.

## Requirements

| What | Tested with |
|---|---|
| Emscripten SDK | 6.0.10 (`emsdk-exec.sh` sources `/opt/Emscripten_SDK/emsdk_env.sh`, adjust the path for your machine) |
| Dear ImGui | 1.93.0, sources in `IMGUI_DIR` (default `/Src_Libs/imgui`, see `Makefile`) |
| GNU make | 4.3 |
| Python 3 | 3.12 (standard library only, used by `pack_worker.py`) |
| Browser | Chromium-based (Chrome/Edge) with WebUSB, served over `https://` or `http://localhost` |
| Adapter | GS-USB compatible firmware (candleLight FD style; hardware timestamps need the `HW_TIMESTAMP` feature), or a PEAK PCAN-USB FD family adapter (not tested on real hardware, see the change history) |

On Linux the adapter usually needs a udev rule that grants your user access to the USB device. PEAK adapters are claimed by the
`peak_usb` / `pcanfd` kernel driver (Linux) or the PEAK driver (Windows) as soon as they are plugged in; WebUSB can use them only when that
driver is not bound to the device (unbind it, or use WinUSB/Zadig on Windows).

## Build

```sh
make -j                        # single (default): build/single/index.html, one self-contained file, also copied to docs/index.html
make -j MODE=multi             # build/multi/: index.html index.js index.wasm usb_worker.js can_core.js can_core.wasm
make serve                     # serve the selected mode on http://localhost:8000/
make clean                     # remove build/
make help                      # all variables
```

`./compile_script.sh [single|multi]` is a thin wrapper for the same (`make -j MODE=...`). Both modes can coexist.

`single` builds the CAN core with `-sSINGLE_FILE`, packs its glue together with `usb_worker.js` into a string
(`pack_worker.py`) and embeds that into the page, which starts the worker from a Blob URL. Deploy `index.html` alone.
`multi` keeps the worker as a separate file; deploy all six files from one directory.

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

All sources and headers are in `src_inc/`:

| File | Role |
|---|---|
| `src_inc/main.cpp` | GLFW/ImGui setup, main loop, wiring of bridge callbacks |
| `src_inc/app_ui.cpp/.h` | All windows, adapter slots (`DeviceSlot`), per-adapter channel start/stop, snapshot ingestion, URL parameters |
| `src_inc/url_params.h` | Parsing of the URL parameter values |
| `src_inc/webusb_bridge.cpp/.h` | Page side of the worker link (EM_JS): device slots, device events, snapshot validation, per-adapter statistics |
| `src_inc/usb_worker.js` | Worker: one `Device` context per adapter (WebUSB I/O, RX slots, control/TX queues, watchdog), snapshot publishing |
| `src_inc/can_core.cpp` | Worker wasm module: frame parsing, ID table keyed by (device, ID), trace queue, per-adapter clock mapping |
| `src_inc/can_protocol_common.h` | Protocol independent definitions: `CanMessage`/`CanPayload`, `CanBittiming`, `CanProtocolType`, DLC helpers, bit-timing calculation, `kMaxDevices` |
| `src_inc/can_protocol_gsusb.cpp/.h` | gs_usb (candleLight / CANable): control requests, structures, bulk frame parsing and building |
| `src_inc/can_protocol_pcan.cpp/.h` | PEAK PCAN-USB FD (uCAN): packed command/record structures, command transfers (clock, options, bit timing, filter, bus on/off), record stream parsing with reassembly across transfers, frame building, bit-timing limits |
| `src_inc/can_protocol.cpp/.h` | Umbrella header and the `CanProtocolHandler` facade: the single entry point the UI and the core use, forwards to the protocol backend |
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

- Chromium only. An adapter is matched by VID/PID/serial. Identical adapters without serial numbers can be told apart only by
  "not yet in use", so after replugging them their device numbers may be swapped.
- Only CAN channel 0 of an adapter is used; multi-channel adapters (gs_usb with several channels, PCAN-USB X6 / Pro FD with two ports) show their
  channel count but are driven as one channel.
- PEAK specifics: the protocol is picked from the USB ids only (no manual override); every transmitted frame requests a TX echo, calibration,
  status and error records are ignored, there is no hardware loopback switch and the CAN clock is fixed at 80 MHz.
- At most 8 adapters (`kMaxDevices` in `can_protocol.h`, `MAX_DEVICES` in `usb_worker.js`, `MAX_DEVICES` in `webusb_bridge.cpp`). The limits
  of the ID table (65536 rows) and the trace are shared by all adapters.
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
(it used no optimization flags before); the single-file `index.html` shrank from about 1.7 MB to 0.87 MB. In the environment it was built in the
`-O2` page could only be checked statically (the optimized JS parses, the Blob-worker start, the snapshot call and the URL reader are present, every identifier
used by the `EM_JS` bodies is still defined); it was then confirmed working on hardware.

**2026-10-27, several adapters.** Before, the whole stack assumed one adapter: one `USBDevice` and one set of receive/TX/control queues in the
worker, one connection and channel state and one bit-timing configuration in the UI, no device number in a frame, a table row per CAN ID.
- *Worker:* `usb_worker.js` was restructured around a `Device` class; there is one context per device number (0..7) with its own `USBDevice`,
  receive slots (generation token), control queue, bounded TX queue, watchdog, recovery and statistics. All messages carry `dev` (and `epoch`
  for the messages that belong to a connection). Opening is serialized across adapters and never hands out a `USBDevice` that another context
  already holds, which lets identical adapters without serial number coexist. An unplug event is matched by object identity first. The bulk
  endpoints and the receive size are now read from the interface descriptor (defaults 1/2/512 when it is not available) and reported to the page.
  The publish timer and the watchdog run while any adapter is open.
- *Core and formats:* frames are tagged with the device number (`CanMessage::dev`, staging records carry it), the ID table key became
  (device, extended flag, ID), the clock mapping is kept per adapter (`core_reset_clock(dev)`), and the snapshot header carries per-device
  frame counters (80 bytes now).
- *Bridge:* the JS side keeps a table of slots (picked `USBDevice` + connection state + epoch). New calls: connect / disconnect / remove per slot;
  every command takes the device number. New events: device added (what the browser knows: name, manufacturer, serial, USB and device revision),
  opened (with the endpoints), closed (with the reason) and a user-facing notice (no free slot, adapter already connected). Statistics are per device.
- *UI:* the connection window and the adapter information were merged into one window with a device list and an information pane (the adapter
  description is read from the adapter with `GS_USB_BREQ_DEVICE_CONFIG` in addition to `BT_CONST`). Bit timing, mode and CAN clock are per adapter;
  the channel start/stop sequence, its acknowledgements and the roll-back on failure are per adapter. The tables got a `Dev` column
  (receive table, trace, periodic jobs, history) and the receive views a device filter. Transmit has a target selector; jobs and history remember
  their target, jobs of a removed adapter are disabled instead of being silently retargeted.
- *Verified:* the worker with three scenarios of mock adapters (independent open, identical adapters without serial, simultaneous reception with the real core,
  per-device rx gate, control isolation with a hung adapter, TX routing and endpoints, unplug, reconnect with a new epoch, recovery and watchdog of one adapter,
  stray commands), also on the packed single-file worker; the core and the bridge's event/statistics handling natively (ASan/UBSan); the UI logic
  (adapter events, per-device channel sequences and acks, send to one/all, history/jobs targets, snapshots with two rows per ID, removal) headless with the real Dear ImGui,
  and its layout by rendering the ImGui draw lists to an image. Not verified: operation with real, simultaneously connected adapters, and the
  JS code of the bridge (chooser, slot table, reconnect) in a real browser: it was only checked statically (it parses and its identifiers are defined).

**2026-10-27, PEAK PCAN-USB FD protocol (stage 2).** The adapter family is selected by the USB ids (`CanDetectProtocol` in C++,
`detectProtocol` in the worker). New `can_protocol_pcan.*` contains the uCAN structures, taken from PEAK's own Linux driver sources
(`pcanfd_usb_fw.h`, `pcanfd_usb.c`, `pcanfd_core_user.h` of peak-linux-driver 9.3.1), not from the draft in `to_merge/`: 28-byte receive and
20-byte transmit records, command opcodes, option bits, frame flags, timing register widths, 64-row acceptance filter, command transfers of at
most 512 bytes (terminated by eight 0xFF bytes unless full). Because a record can be split between two USB transfers, the core keeps a receive-stream
state per adapter (`CanRxStreamState`, reset on open, when the receive gate is toggled and on recovery). The worker performs the open handshake
(vendor requests "driver loaded" and "firmware info" with recipient *other*), takes the endpoint numbers from the firmware info, sends command
transfers in order (`cmd_out`, acknowledged with the command id) and probes liveness with the firmware-info read. The UI starts a PEAK channel by
writing the command transfers (clock, options, nominal and data timing, filter, bus on) and treats the channel as open when the last one was
written; bit timing is calculated from the fixed 80 MHz clock within PEAK's limits (`CanBitTimingLimits`). gs_usb behaviour is unchanged (digests
of about 400 000 receive transfers and all timing/DLC cases are identical to the pre-stage-2 code). Verified without hardware: structure sizes and
field offsets against the PEAK headers, command byte streams, record parsing including fragmentation and a fuzzer under ASan/UBSan (one defect found
and fixed there: a negative `memcpy` size on an inconsistent stream state), the core with a PEAK adapter next to a gs_usb one, the worker against
mock `USBDevice`s (handshake, endpoints from the info record, command order and failure ack, fragmented stream, stream reset, handshake failure,
liveness probe), also with the packed single-file worker, the UI sequences (start/stop/rollback, TX bytes, timing limits) headless, and both build
modes without warnings. **Not verified: any real PEAK adapter.** Timestamps are treated as microseconds as in PEAK's driver. Adapters with
several CAN ports are the next stage.

**2026-10-27, protocol layer split (stage 1 of adding a second protocol).** `can_protocol.h/.cpp` held both the protocol independent types
and everything gs_usb specific. They were split into `can_protocol_common.h` (types, bit-timing and DLC helpers) and `can_protocol_gsusb.h/.cpp`
(gs_usb requests, structures, frame parsing/building), following the structure of the proposal in `to_merge/`, but based on the current code
(that proposal predates the `CanPayload`/`dev` types and the multi-frame parser). `can_protocol.h` stays the entry point: `CanProtocolHandler`
keeps its API and forwards to the gs_usb backend, so the UI and the core did not change; `GsDeviceBittiming` is an alias of the generic
`CanBittiming` (same 20-byte layout). Behaviour is unchanged, checked by running the old and the new implementation on the same inputs
(about 400 000 random and malformed receive transfers, 200 000 frames to build, all bit-timing and DLC cases) and comparing digests: identical.
One defect was fixed on the way: a frame announcing more data than it holds (for example a 24-byte transfer with the FD flag and DLC 15, or such
a frame inside a multi-frame transfer) made the parser read up to 64 bytes beyond the frame (AddressSanitizer: heap-buffer-overflow). The parser
now copies what is there and pads the payload with zeros; frames that are well formed are not affected. The next stages are the PCAN-USB FD protocol
and adapters with several CAN ports (the first is done, see above).

What was and was not verified overall: the CAN core, frame parser and URL parsing were unit-tested natively (ASan/UBSan) and the worker was exercised
against a mock `USBDevice` with the real core module in Node; the single-file Blob-Worker and the `-O2` page were confirmed on hardware with one adapter.
Throughput numbers and the accuracy of hardware timestamps were not measured.

## License

MIT, see `LICENSE`.
