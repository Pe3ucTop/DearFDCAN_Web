# Decisions: splitting the project into libraries and a utility

Status: agreed 2026-10-30, work in progress. This file records what was decided and why; `ChangeLog.md` records what was done.

## Goal

Split the monitor into three layers, so that the parts can be reused and tested on their own:

```
 3. the utility (Dear ImGui): windows, per-adapter settings, URL parameters, main loop
 2. CAN libraries (C++17, no Emscripten): can-core -> can-protocols -> can-engine -> can-adapters
 1. transport: USB / Serial (JavaScript core + Emscripten wrappers)
```

Dependencies point downwards only. Layer 2 reaches the hardware through an interface (`ITransport`), which layer 1 implements.

## Decisions

| # | Question | Decision | Reason |
|---|---|---|---|
| 1 | Audience of the USB wrapper | **Third-party projects.** Own C API (`webusb.h`) modelled on WebUSB. **No libusb compatibility** for now. | libusb semantics would add constraints (synchronous event loop, ASYNCIFY / JSPI) that nothing here needs. A compatible layer can be added on top later. |
| 2 | Build system | **CMake becomes the source of truth.** The `Makefile` stays as a thin front-end with the same commands (`make`, `make MODE=multi`, `make serve`, `make clean`) and calls CMake. **`compile_script.sh` is dropped** when CMake lands. | CMake gives one place that describes the libraries, native test targets and the Emscripten targets. The script only wraps `make -jN MODE=...` (it adds a `make` check and `nproc`), nothing else refers to it except the README table, so it carries no logic that has to be kept. |
| 3 | Single-file `index.html` | **Kept** for the utility (GitHub Pages). The multi-file mode stays too. | `pack_worker.py` must keep packing the JS modules into the page; each new JS module has to be an input of that step. |
| 4 | Targets | **Web only.** No native builds of the utility or of the transports. The CAN libraries stay free of Emscripten headers, but only so that their unit tests run natively. | Native transports mean a second backend for everything; not wanted. |
| 5 | Repository | **Monorepo**, one directory per component; split into repositories later (`git subtree`) once the interfaces have settled. | Interfaces still change. |
| 6 | Licence and file headers | Apache-2.0 for every component. Per-file headers are **postponed** until the layout is final. | Headers name the component, so they are written once. |

## Target layout (step 1 is in place; the JavaScript transport libraries of steps 2 and 3 are not)

```
libs/emscripten-webserial/   Web Serial ports as /dev/ttyWebN
libs/webusb-io/              the WebUSB transport (JS)                                (step 2, done)
libs/webserial-io/           the Web Serial transport (JS)                            (step 2, done)
libs/emscripten-webusb/      library_webusb.js + webusb.h on top of webusb-io         (step 3, not yet)
libs/can-core/               types, DLC, bit timing                  (can_protocol_common.h)
libs/can-protocols/          gs_usb, Elmue, PEAK, SLCAN              (can_protocol_*.cpp/.h)
libs/can-engine/             clock sync, ID table, trace, snapshots  (can_core.cpp, can_wire.h)
libs/can-adapters/           per-adapter drivers on ITransport       (logic now inside app_ui.cpp)
apps/dearfdcan_web/          the ImGui utility
tests/                       per library, run natively / under Node
```

## Interfaces

* **`ITransport`** (C++, per adapter): control out/in, command out, start/stop of the receive pump. Implemented by the page's bridge to the worker.
* **Worker message protocol** (`usb_worker.js`: `open`, `ctrl_out`, `ctrl_in`, `cmd_out`, `tx`, `rx`, ...): the contract between the C++ side and the workers; specified in `apps/dearfdcan_web/WORKER_PROTOCOL.md`.
* **`UsbPort` / `SerialConnection`** (JavaScript): the transport classes of layer 1, extended by subclassing (`UsbPort` hooks) or by a hooks object (`SerialConnection`); described in the READMEs of the two libraries.
* **Engine C API** (`core_*`): to be given a prefix and a version before it is published.
* **`AdapterDriver`** (new, step 0): everything that is protocol knowledge and today sits in `AppUI`.

## PEAK note

The PEAK record layouts and option bits come from PEAK's Linux driver (GPL). Only the data formats are used. When `can-protocols` is split out, PEAK stays a separate module that can be left out of a build, and the README of the library states the origin of the layouts.

## Steps

0. **Prepare without moving files** (done 2026-10-30): the protocol logic moved out of `AppUI` into drivers over `ITransport` (`can_adapter.*`); `usb_worker.js` split into one class per protocol; the headless tests of the start sequences are in the repository (`tests/`).
1. **CAN libraries** (done 2026-10-30): moved to `libs/` (`can-core`, `can-protocols`, `can-engine`, `can-adapters`), the monitor to `apps/dearfdcan_web/`, `emscripten_webserial` to `libs/emscripten-webserial/`; CMake describes the build, the `Makefile` is its front-end; native unit tests under CTest (sanitizers on); `compile_script.sh` removed. The move itself is `tools/restructure.sh`, to be run once and deleted.
2. **JavaScript transport** (done 2026-10-30): `libs/webusb-io` (`UsbPort`) and `libs/webserial-io` (`SerialConnection`) with mock-device tests; `usb_worker.js` keeps what is specific to the CAN adapters (the page's messages, the link to the core, gs_usb clock requests, the PEAK handshake) on top of `UsbPort`, `serial_worker.js` is a shell around `SerialConnection`; the messages are specified in `apps/dearfdcan_web/WORKER_PROTOCOL.md`. The libraries are loaded with `importScripts` (multi-file) or packed in front of the worker (single-file, `pack_worker.py`).
3. `library_webusb.js` + `webusb.h` (own API) with a test like the one of `emscripten-webserial`.
4. The utility on the new libraries; check the single-file build (`pack_worker.py` with several modules). The layout already builds the single-file page from the CMake targets; what stays is to let the page depend on the JavaScript transport libraries once they exist.

Native transports and a console utility are **out of scope**.
