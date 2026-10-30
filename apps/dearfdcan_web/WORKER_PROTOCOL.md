# The message protocol of the workers

The page (Emscripten wasm and Dear ImGui) talks to two dedicated workers only through `postMessage`. There is no `SharedArrayBuffer` and no
`Atomics`. An `ArrayBuffer` that is sent is **moved** with the transfer list, so two contexts never touch the same bytes.

```
 page ──────────── messages (§1, §2) ────────────► usb_worker.js   (CAN worker: USB adapters, the core, the snapshots)
  │                                                    ▲   │
  │                                  MessageChannel    │   │ sopen / sclose / swrite       (§3)
  └── link, pick, forget (§4) ──► serial_worker.js ────┘   ▼ sopened / sclosed / srx / swack / sstats
                                    (Web Serial I/O only)
```

Every message has a string `type`. Messages about one adapter carry `dev`, the device number (slot) `0..7`, chosen by the page
(`kMaxDevices`). **`epoch`**: the page numbers every connection of a slot; the worker puts the epoch of the `open` it is working on into all its
answers, and the page ignores answers whose epoch is not the current one of the slot. A late answer of a closed connection can therefore never
be mixed up with the new one. Snapshots belong to no connection (the ID table and the trace live across connections) and have no epoch.

The transport libraries (`libs/webusb-io`, `libs/webserial-io`) know nothing of these messages; this file is the contract of the monitor's two
workers.

## 1. Page → CAN worker (`usb_worker.js`)

| Message | Fields | Meaning |
|---|---|---|
| `init` | `timeOrigin` | `performance.timeOrigin` of the page. All times in the worker's answers are on the page's `performance.now()` timeline. |
| `serial_link` | `port` | The worker's end of the `MessageChannel` to the serial worker (transferred). |
| `open` | `dev, epoch, vid, pid, serial?, protocol?, transport?, baud?` | Opens the adapter of the slot. USB: the device must already be permitted (`requestDevice()` is window-only); it is found with `getDevices()` by `vid/pid` (and `serial` if given) among the devices no other slot holds. `protocol` forces `0` gs_usb / `1` PEAK, else it follows from the USB ids. `transport: 'serial'` opens the picked serial port with `baud`. A slot changes its context class when the kind of adapter changes. Answer: `opened` or `closed`. |
| `close` | `dev` | Closes the connection (answer `closed` with reason `closed by host`). |
| `ctrl_out` | `dev, request, value, index, data, id?, reqType?, recipient?` | USB control OUT (default: vendor request, recipient interface). Executed **strictly in order** with the other control and command transfers of the slot, each with a timeout. Answer: `ctrl_ack` with `request` = `id` if `id >= 0`, else the request code. |
| `ctrl_in` | `dev, request, value, index, length, reqType?, recipient?` | USB control IN. Answer `ctrl_in` with the bytes; no answer if the transfer failed or returned nothing. SLCAN adapters have no control pipe (a `ctrl_out` is answered with a failed `ctrl_ack`). |
| `cmd_out` | `dev, id, ep, data` | A command to the adapter: bulk OUT to the command endpoint (PEAK; `ep` 0 = the one found at open), or one `\r`-terminated line to a serial adapter. Ordered with the control transfers; answer `ctrl_ack` with `request` = `id`. SLCAN: the answer is the adapter's own reply; the version request (`id` 0x56, `V`) is answered with its text line as `ctrl_in`. |
| `tx` | `dev, ep, data` | A frame to send: bounded queue, a limited number of transfers in flight; `ep` 0 = the bulk OUT endpoint found at open. No answer; a full queue or a closed adapter counts in `txDropped`. |
| `rx` | `dev, enable, ep, size, elmue?, ts?` | The **receive gate**. The worker keeps receive transfers pending as long as the adapter is open; the gate decides whether what arrives is processed. Enabling resets the clock mapping and the record stream of the slot. `ep` / `size` (0 = keep) set the endpoint and transfer size of the receive transfers (a change replaces the pending transfers). gs_usb: `elmue` = the channel runs in the Elmue protocol (compact messages), `ts` = it was started with adapter timestamps, which makes the worker read the adapter's clock once a second. |
| `core` | `op, arg` | Commands for the CAN core, run after the already received frames were processed: `op` 1 = clear the ID table, 2 = clear the trace, 3 = trace on/off (`arg`). |
| `ret` | `buf` | A snapshot buffer handed back. One flow-control credit (§2). |

## 2. CAN worker → page

| Message | Fields | Meaning |
|---|---|---|
| `opened` | `dev, epoch, eps, rxEp, txEp, protocol, info` | The adapter is open. `eps` = text of the endpoints for the information pane, `protocol` = `CanProtocolType` (0 gs_usb, 1 PEAK, 2 SLCAN), `info` = `key=value;key=value` (PEAK: `fw bl hw mcu ser ch fwtype`; serial USB ports: `vid pid`). |
| `closed` | `dev, epoch, reason` | The connection ended: closed by the page (`closed by host`), an open that failed (`open failed: …`), or lost (unplugged, recovery failed, …). |
| `snapshot` | `buf, used` | The changed ID-table rows and the new trace entries since the previous snapshot, `SnapshotHeader` followed by the records (`libs/can-engine/can_wire.h`). At most one per 16 ms. **Credits:** at most 4 snapshots may be unacknowledged; the page returns each buffer with `ret`. Without a credit nothing is published and the core keeps aggregating (rows of one ID coalesce, the trace queue is bounded), so a slow page costs resolution but never memory. |
| `ctrl_in` | `dev, epoch, request, data` | The bytes of a control IN (or the text line of the SLCAN version command). |
| `ctrl_ack` | `dev, epoch, request, ok` | A control OUT or command finished (`request` as in §1). A command that is still waiting when the connection ends is answered with `ok: false`. |
| `stats` | `dev, epoch, rxPerSec, txDropped, recoveries, rxErrors, txErrors` | Once a second while the adapter is open. |
| `sync` | `dev, epoch, s` | Statistics of the clock samples of the adapter, 9 numbers: samples, good samples, offset (ms), drift (ppm, + = the adapter runs fast), spread (rms µs), shortest round trip (ms), last round trip (ms), worst deviation (µs), 1 = the samples are records the adapter pushes (PEAK; the "round trip" is then the delay above the lowest). gs_usb: after every answered `GS_USB_BREQ_TIMESTAMP`; PEAK: when a calibration record arrived. |
| `events` | `dev, epoch, text` | Lines the core collected besides frames, `\n` separated: `U<percent>` bus load, `S<0..3>` bus state, `T<text>` text of the firmware, `E<id 8 hex>:<data 16 hex>` an error report. Elmue adapters and PEAK. |

## 3. CAN worker ↔ serial worker (over the `MessageChannel`)

CAN worker → serial worker:

| Message | Fields | Meaning |
|---|---|---|
| `sopen` | `dev, epoch, baud` | Open the picked port of the slot and start reading. The previous session of the slot is cleaned up first. |
| `sclose` | `dev` | Close the port. |
| `swrite` | `dev, epoch, data, id` | Write the bytes in order. A write with another epoch is refused. `id != 0` is acknowledged with `swack`. |

Serial worker → CAN worker:

| Message | Fields | Meaning |
|---|---|---|
| `sopened` | `dev, epoch, eps, info` | The port is open and reading. |
| `sclosed` | `dev, epoch, reason` | The port is closed or lost. |
| `srx` | `dev, epoch, buf, t` | Bytes read, moved, never copied. `t` = `performance.timeOrigin + performance.now()` when the read completed. |
| `swack` | `dev, epoch, id, ok` | The write with this id finished (or was dropped / never ran: `ok: false`). |
| `sstats` | `dev, epoch, rx, rxErrors, txErrors, txDropped` | About once a second while the port is open. |

SLCAN replies carry no id: they come in the order of the commands. The CAN worker therefore keeps the commands that wait for a reply in a
queue and matches the reply tokens of the core (`K` ok, `E<code>` error, `L<text>` any other line) to its head; `C\r` (close) is never
answered; a reply that does not come within 1.5 s fails the command.

## 4. Page → serial worker (`serial_worker.js`)

| Message | Fields | Meaning |
|---|---|---|
| `link` | `port` | The serial worker's end of the `MessageChannel` (transferred). |
| `pick` | `dev, port` or `dev, vid, pid, idx` | The port the user chose for the slot. If `SerialPort` objects can be transferred, `port` is the object; otherwise the port is found again with `navigator.serial.getPorts()` as the `idx`-th permitted port with these USB ids. |
| `forget` | `dev` | Drop the port of a removed slot. |

## 5. The staging records of the core

Received bytes go from the worker into the core without being parsed in JavaScript: each chunk is copied into the core's staging buffer as
`[u16 length][u8 dev][u8 endpoint address][f64 host time ms][bytes]`, `host time` on the page's timeline. An endpoint address with bit 7 set is
a receive (IN) chunk; without it the record is a message the host sent, which only the Elmue protocol needs (the core keeps the frame until its
echo comes). `core_process()` parses the staged records of all adapters in order.
