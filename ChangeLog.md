# Change log

What the code looked like before the rework and what was actually changed, written from the code itself
(the git history was created afterwards and does not follow these steps). Entries are in the order the work was done.

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
experiments in the build script were removed, `compile_script.sh` usage text matches its name, the README was written.

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

**2026-10-27, protocol layer split (stage 1 of adding a second protocol).** `can_protocol.h/.cpp` held both the protocol independent types
and everything gs_usb specific. They were split into `can_protocol_common.h` (types, bit-timing and DLC helpers) and `can_protocol_gsusb.h/.cpp`
(gs_usb requests, structures, frame parsing/building), following the structure of the proposal in `to_merge/`, but based on the current code
(that proposal predates the `CanPayload`/`dev` types and the multi-frame parser). `can_protocol.h` stays the entry point: `CanProtocolHandler`
keeps its API and forwards to the gs_usb backend, so the UI and the core did not change; `GsDeviceBittiming` is an alias of the generic
`CanBittiming` (same 20-byte layout). Behaviour is unchanged, checked by running the old and the new implementation on the same inputs
(about 400 000 random and malformed receive transfers, 200 000 frames to build, all bit-timing and DLC cases) and comparing digests: identical.
One defect was fixed on the way: a frame announcing more data than it holds (for example a 24-byte transfer with the FD flag and DLC 15, or such
a frame inside a multi-frame transfer) made the parser read up to 64 bytes beyond the frame (AddressSanitizer: heap-buffer-overflow). The parser
now copies what is there and pads the payload with zeros; frames that are well formed are not affected.

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
several CAN ports are a later stage.

**2026-10-27, GitHub Pages.** `make` (single mode) also copies the page to `docs/index.html` (plus `docs/.nojekyll`), the folder GitHub Pages serves.

**2026-10-28, SLCAN over Web Serial, a serial worker and `emscripten_webserial`.** New protocol `can_protocol_slcan.*`, following the
CANable 2.5 firmware sources (`Firmware/Slcan/control.c`, `buffer.c`, `settings.h`) rather than the draft in `external_resources/Emscripten_WebSerial_ext`,
whose parser used the wrong frame letters for CAN FD (`r`/`R` instead of `d D b B`), folded lower case into upper case (so every standard ID looked
extended) and invented flag characters. Frame lines are `t T r R d D b B` with an optional `S` (ESI) suffix; commands are answered by `\r` / BEL, or
`#\r` / `#<code>\r` in feedback mode (`MF`), and `V` answers a line with the clock and the bit-timing limits that the UI uses for the exact timing
commands `s<brp>,<seg1>,<seg2>,<sjw>` and `y...`; the start sequence is `C`, `MF`, `MS`, `s`, `y`, `O`/`OS`/`OI`. Lawicel adapters get `S<code>` and `O`/`L`.
The core keeps a line reassembly state per adapter (the stream state grew to 256 bytes: the `V` line can be 199 characters), reports non-frame
lines as reply tokens, can gate frames per adapter and spreads the frame times of a serial chunk over the interval since the previous chunk. Serial I/O
is a separate worker (`serial_worker.js`), connected to the CAN worker by a `MessageChannel` (decision: the CAN worker stays asymmetric, it keeps the USB
path and hosts the core, so USB transfers take no extra hop; a benchmark under Node put one hop at roughly 6-12 µs of CPU per transfer and about
0.2 ms of latency, which would have been acceptable but buys no speed). The UI got **Add Serial Port**, baud rate, dialect choice and a bit-rate list
for classic adapters. `emscripten_webserial/` is a separate reusable library that exposes Web Serial ports as `/dev/ttyWebN` device nodes (found while
reading the Emscripten sources: `FS.registerDevice`, a `poll` stream op, and `stream.tty` for termios). Verified without hardware: parser and
builder against a port of the firmware's formatter, chunking at every offset, a fuzzer under ASan/UBSan, the core, the UI sequences (probe, both start
sequences, rejected command, FD refused on a classic adapter, stop) headless, a screenshot of the Devices window, the workers against mock
`SerialPort`s on Web Streams with an adapter simulator (open, `V`, start sequence, error replies, chunked reception, gate, TX, reply timeout, unplug,
reopen, `getPorts()` fallback, two dialects) also with the packed single-file workers, and the library against a mock `navigator.serial`.
**Not verified: any real SLCAN adapter, Web Serial inside a worker in a real browser, transferability of `SerialPort`, and the library in a browser.**

**2026-10-29, ElmueSoft CANable 2.5 extensions of gs_usb and clock synchronisation.** Checked the gs_usb layer against the firmware's
`candlelight_def.h`: the program spoke only the legacy protocol. New `can_protocol_elmue.*`: the compact bulk messages (`kHeader` size+type; rx
frame 7 bytes + data, tx frame 8 bytes + data, with/without the 4 byte timestamp), blobs (`frame_count` + `MSG_RxBlob`, up to 2048 bytes), tx
echoes by marker, error reports, bus load and text messages, plus the control requests `BT_CONST_EXT` (separate limits of the data phase, which the
timing calculation now respects), board info, host filters, bus load report, termination and the feedback read (`GET_LAST_ERROR`). The extended
requests only work while the last `MODE` request carried `ELM_DevFlagProtocolElmue`, so the open sequence and every stop send a mode reset with
the flag, and the start sequence is: host format, reset, bit timing(s), filters, bus load, termination, start (flags Elmue + USB blobs + timestamps),
each required step followed by a feedback read because the firmware cannot stall the data stage to report a rejected parameter (the UI then stops
the channel and says what was rejected). Control OUT transfers can now be tagged with an id (acknowledged as that id), since `MODE` is sent twice.
In the core `PROTOCOL_GS_USB_ELMUE` is selected per channel start; the worker stages every transmit message so the core can turn a tx echo (marker
only) back into the sent frame; error reports become frames (`is_error`, own table rows, decoded in the trace) and bus load / text / error lines
go to the page as events, which show up in the device pane (bus state, error counters, bus load, last error). UI: Elmue protocol and HW
timestamps switches, bus load report, termination switch, up to eight hardware ID filters, board information. gs_usb receive transfers are now
2048 bytes for every gs_usb adapter (no re-claim when an Elmue channel starts). Not implemented: transmit blobs (they need flow control from the
echo markers), bridge filters of multi-channel boards, flash / pin requests.
Clock synchronisation: while a gs_usb channel runs with hardware timestamps the worker reads `GS_USB_BREQ_TIMESTAMP` (a burst of 8 requests
after the start, then one per second) and measures each round trip; the core keeps the last 64 samples (offset = page time in the middle of the
request minus adapter time), keeps the ones with the shortest round trips, fits a line (offset and drift, slope limited to 500 ppm, fitted only over
at least 4 s) and reports the drift, the rms spread and the worst deviation of the used samples; with three good samples the frame times follow the
line (never later than the moment the frame was handled), before that the old latency minimum is used. The Devices window shows the numbers. A
timestamp slightly older than the newest one seen (a frame behind a clock sample) is jitter now, not an adapter restart. Verified without hardware:
parser, builder, blobs, echoes and a fuzzer under ASan/UBSan (10 712 checks), the core with stage records, echoes and events, the clock fit against
a simulated clock with 80 ppm drift, noisy and sometimes long round trips and a 32 bit wrap (drift estimated 82 ppm, spread 0.26 ms, frame times
within 0.07 ms), the worker against a mock Elmue `USBDevice` (2048 byte transfers, tx staging, blob with echo / bus load / error / frame, events,
sync burst and stop, switch back to legacy), the UI sequences headless with real Dear ImGui (open, capabilities, Elmue start with filters,
feedback rejection, failed required / optional transfers, legacy start byte for byte as before, TX bytes, events, rendering) and both build modes
without warnings. **Not verified: any real CANable 2.5 or other Elmue adapter.**

## Verified overall

The CAN core, frame parser and URL parsing were unit-tested natively (ASan/UBSan) and the worker was exercised against mock devices with the
real core module in Node; the single-file Blob-Worker and the `-O2` page were confirmed on hardware with one adapter. Throughput numbers and the
accuracy of hardware timestamps were not measured.
