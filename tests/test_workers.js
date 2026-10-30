'use strict';
// Tests of usb_worker.js (the real worker and the real can_core.wasm) against mock USB adapters:
//   * a CANable 2.5 (gs_usb, Elmue protocol): receive transfers of 2048 bytes, transmit staging, blob with echo / bus load / error / frame,
//     event lines, the clock requests and their statistics, the switch back to the legacy protocol
//   * a PEAK PCAN-USB FD: records with calibration, bus load, error and status; no clock request
//   * a slot that changes from one adapter kind to another
// Usage: node tests/test_workers.js <build/multi directory> <directory of usb_worker.js, apps/dearfdcan_web> [build/tmp/single/usb_worker_bundle.js]
// With the third argument the same checks run against the packed worker of the single-file build (core, libraries and worker in one script).
const { sleep, createChecks, createMockDevice, createHarness, le32 } = require('./worker_harness');
const BUILD = process.argv[2], SRC = process.argv[3];
const PACKED = process.argv[4] || null;     // optional: the --pre-js file of the single-file build (build/tmp/single/usb_worker_bundle.js)
const C = createChecks();
const check = C.check;

async function testGsUsbElmue() {
    let adapterBase = 1_000_000;
    const mock = createMockDevice(0x1D50, 0x606F, 'ELM1', (setup, len, st) => {
        const dv = new DataView(new ArrayBuffer(len));
        if (setup.request === 6) dv.setUint32(0, (adapterBase + Math.round((performance.now() - st.t0) * 1000 * 1.00005)) >>> 0, true);
        return dv;
    });
    const adapterUs = () => (adapterBase + Math.round((performance.now() - mock.t0) * 1000 * 1.00005)) >>> 0;
    const h = createHarness(BUILD, SRC, mock, PACKED);
    h.send({ type: 'init', timeOrigin: performance.timeOrigin });
    h.send({ type: 'open', dev: 0, epoch: 1, vid: 0x1D50, pid: 0x606F, serial: 'ELM1' });
    const opened = await h.waitFor(m => m.type === 'opened', 3000, 'opened', C);
    check(opened && opened.protocol === 0, 'gs: opened as gs_usb');

    // the channel start in Elmue mode: the page asks for 2048 byte transfers, which is the size of the pending ones already
    h.send({ type: 'rx', dev: 0, enable: true, ep: 0, size: 2048, elmue: true, ts: true });
    await sleep(50);
    check(mock.rxWaiters.length === 6 && mock.rxWaiters.every(w => w.len === 2048), 'gs: six transferIn of 2048 bytes pending (got ' + mock.rxWaiters.map(w => w.len) + ')');

    // TX: the page stages an Elmue transmit message; the worker hands it to the core and to the bus
    const txMsg = new Uint8Array([9, 10, 0, 0x22, 0x02, 0, 0, 0x2B, 0xEE]);
    h.send({ type: 'tx', dev: 0, ep: 0, data: txMsg.buffer.slice(0) });
    await sleep(30);
    check(mock.txOut.length === 1 && mock.txOut[0].ep === 2 && mock.txOut[0].data.length === 9, 'gs: tx transfer reached the endpoint');

    // RX transfer: a blob with the echo of that frame, bus load, an error report and a received frame (all with timestamps)
    const echoMsg = [7, 11, 0x2B]; le32(echoMsg, adapterUs());
    const busload = [3, 15, 55];
    const err = [0, 13]; le32(err, 0x80); err.push(0, 0x08, 0x04, 0, 0, 0, 5, 0); le32(err, adapterUs()); err[0] = err.length;
    const frame = [0, 12, 0]; le32(frame, 0x1AA); le32(frame, adapterUs()); frame.push(9, 8, 7); frame[0] = frame.length;
    mock.pushRx([4, 17, ...echoMsg, ...busload, ...err, ...frame]);
    const snap = await h.waitFor(m => m.type === 'snapshot', 2000, 'snapshot', C);
    const events = await h.waitFor(m => m.type === 'events', 2000, 'events', C);
    check(events && /U55\n/.test(events.text) && /E00000080:0008040000000500\n/.test(events.text), 'gs: events ' + (events && JSON.stringify(events.text)));
    if (snap) {
        const dv = new DataView(snap.buf, 0, snap.used);
        const nTable = dv.getUint32(8, true), nTrace = dv.getUint32(12, true), tRec = dv.getUint32(16, true), trRec = dv.getUint32(20, true);
        check(nTrace === 3, 'gs: three trace entries (' + nTrace + ')');
        const base = 80 + nTable * tRec;
        const entry = i => { const o = base + i * trRec + 8; return { id: dv.getUint32(o, true), echo: dv.getUint8(o + 21), hw: dv.getUint8(o + 22), err: dv.getUint8(o + 23), marker: dv.getUint8(o + 24), data0: dv.getUint8(o + 27), len: dv.getUint8(o + 27 + 64) }; };
        const e0 = entry(0), e1 = entry(1), e2 = entry(2);
        check(e0.echo === 1 && e0.id === 0x222 && e0.data0 === 0xEE && e0.marker === 0x2B && e0.hw === 1, 'gs: echo resolved to the sent frame ' + JSON.stringify(e0));
        check(e1.err === 1 && e1.id === 0x80, 'gs: error entry');
        check(e2.echo === 0 && e2.id === 0x1AA && e2.len === 3 && e2.data0 === 9, 'gs: rx frame ' + JSON.stringify(e2));
    }

    // clock synchronisation: a burst of requests, then statistics messages
    await sleep(1500);
    const tsReq = mock.ctrlLog.filter(c => c.request === 6);
    check(tsReq.length >= 8, 'gs: timestamp requests during the burst: ' + tsReq.length);
    const syncs = h.posted.filter(m => m.type === 'sync');
    check(syncs.length === tsReq.length || syncs.length === tsReq.length - 1, 'gs: one sync message per answered request (' + syncs.length + ' / ' + tsReq.length + ')');
    const last = syncs[syncs.length - 1];
    check(last && last.s[0] === syncs.length && last.s[1] >= 3 && last.s[5] > 0 && last.s[5] < 20 && last.s[8] === 0, 'gs: sync statistics look sane ' + JSON.stringify(last && last.s.map(x => +x.toFixed(2))));
    check(!mock.ctrlLog.some(c => c.request === 4), 'gs: no liveness probe needed while the clock requests are answered');

    // stop: no more requests
    h.send({ type: 'rx', dev: 0, enable: false });
    await sleep(100);
    const n = mock.ctrlLog.filter(c => c.request === 6).length;
    await sleep(1300);
    check(mock.ctrlLog.filter(c => c.request === 6).length === n, 'gs: no timestamp requests after the channel stopped');

    // legacy channel (no Elmue): a frame in the 20 byte legacy format is parsed, no clock requests without the timestamp flag
    h.posted.length = 0;
    h.send({ type: 'rx', dev: 0, enable: true, ep: 0, size: 0, elmue: false, ts: false });
    await sleep(30);
    const legacy = new Uint8Array(20); new DataView(legacy.buffer).setUint32(0, 0xFFFFFFFF, true); new DataView(legacy.buffer).setUint32(4, 0x55, true); legacy[8] = 2; legacy[12] = 0xA1; legacy[13] = 0xA2;
    mock.pushRx(legacy);
    const snap2 = await h.waitFor(m => m.type === 'snapshot', 2000, 'legacy snapshot', C);
    check(snap2 && new DataView(snap2.buf).getUint32(12, true) === 1, 'gs: legacy frame parsed after switching back');
    await sleep(1300);
    check(mock.ctrlLog.filter(c => c.request === 6).length === n, 'gs: no timestamp requests without the ts flag');
    h.send({ type: 'close', dev: 0 });
    await sleep(50);
    return h;
}

// Elmue transmit: queued frames go out as TX blobs, and no more frames than the adapter's FIFO has free slots are on the wire
async function testElmueTxBlobs() {
    const mock = createMockDevice(0x1D50, 0x606F, 'ELM2', () => null);
    const h = createHarness(BUILD, SRC, mock, PACKED);
    h.send({ type: 'init', timeOrigin: performance.timeOrigin });
    h.send({ type: 'open', dev: 0, epoch: 1, vid: 0x1D50, pid: 0x606F, serial: 'ELM2' });
    await h.waitFor(m => m.type === 'opened', 3000, 'opened', C);
    h.send({ type: 'rx', dev: 0, enable: true, ep: 0, size: 2048, elmue: true, ts: false, blobs: true });
    await sleep(50);

    // the frames in the transfers written so far: [{marker, id}] in the order of the wire; blobs are checked for their format
    let badBlob = 0, blobs = 0, singles = 0;
    const wire = () => {
        const frames = [];
        badBlob = 0; blobs = 0; singles = 0;
        for (const t of mock.txOut) {
            const d = t.data;
            if (d[1] === 10) { singles++; frames.push({ marker: d[7], id: d[3] | (d[4] << 8) }); continue; }
            if (d[1] !== 16 || d.length > 2048) { badBlob++; continue; }
            blobs++;
            let off = 2, n = 0;
            while (off < d.length) { const sz = d[off]; if (sz < 8 || d[off + 1] !== 10) { badBlob++; break; } frames.push({ marker: d[off + 7], id: d[off + 3] | (d[off + 4] << 8) }); off += sz; n++; }
            if (off !== d.length || n !== d[0]) badBlob++;
        }
        return frames;
    };
    const frameMsg = i => new Uint8Array([9, 10, 0, i & 255, 0x01, 0, 0, i, 0xA0 + (i & 15)]).buffer;
    const sendFrames = (from, to) => { for (let i = from; i <= to; i++) h.send({ type: 'tx', dev: 0, ep: 0, data: frameMsg(i) }); };
    const echoes = (from, to) => { const m = [0, 17]; for (let i = from; i <= to; i++) m.push(7, 11, i, 0, 0, 0, 0); m[0] = to - from + 1; return m; };

    // a burst: whatever queued up behind the first transfers is merged; every frame on the wire once, in order
    sendFrames(1, 30);
    await sleep(100);
    let f = wire();
    check(f.length === 30 && f.every((x, i) => x.marker === i + 1 && x.id === 0x100 + i + 1), 'txblob: 30 frames on the wire in order (' + f.length + ')');
    check(blobs >= 1 && badBlob === 0, 'txblob: ' + blobs + ' blob(s) and ' + singles + ' single transfers, well formed (bad: ' + badBlob + ')');

    // 100 frames while no echo comes back: the adapter's FIFO takes 64
    mock.txOut.length = 0;
    sendFrames(31, 130);
    await sleep(200);
    f = wire();
    check(f.length === 34, 'txblob: only the free FIFO slots are used (' + f.length + ' more frames after 30 unechoed, 34 expected)');
    check(badBlob === 0, 'txblob: blobs well formed while limited');

    // the echoes of 1..30 free their slots: more frames go out (the rest stays out while 64 are unechoed)
    mock.pushRx(echoes(1, 30));
    await sleep(200);
    f = wire();
    check(f.length === 64, 'txblob: echoes free slots, 64 frames on the wire in total (' + f.length + ')');
    check(f.every((x, i) => x.marker === i + 31) , 'txblob: order kept across the limit');

    // no echoes at all: after the stall time the rest goes out and the worker says so
    await sleep(1300);
    f = wire();
    check(f.length === 100, 'txblob: stalled echoes are given up, the remaining frames go out (' + f.length + ')');
    check(h.posted.some(m => m.type === 'log' && /did not free/.test(m.text)), 'txblob: the stall is reported in the log');

    // the statistics tell how the transmit path worked: transfers, frames in them, and the time the FIFO gate held the queue back
    await sleep(1100);
    const stats = h.posted.filter(m => m.type === 'stats');
    const lastStats = stats[stats.length - 1];
    check(lastStats && lastStats.txFrames >= 130 && lastStats.txXfers > 0 && lastStats.txXfers < lastStats.txFrames, 'txblob: statistics count transfers and the frames in them (' + JSON.stringify(lastStats) + ')');
    check(lastStats && lastStats.txGateMs >= 900, 'txblob: statistics count the time the FIFO gate held the queue (' + (lastStats && lastStats.txGateMs) + ' ms)');

    // a legacy-like channel (no blobs flag): one frame per transfer
    h.send({ type: 'rx', dev: 0, enable: false });
    await sleep(30);
    h.send({ type: 'rx', dev: 0, enable: true, ep: 0, size: 2048, elmue: true, ts: false });
    await sleep(30);
    mock.txOut.length = 0;
    sendFrames(200, 210);
    await sleep(100);
    wire();
    check(blobs === 0 && singles === 11, 'txblob: without the blobs flag every frame is its own transfer (' + singles + ')');

    // the page can lower the number of FIFO slots the worker fills (11 frames are unechoed now: 16 slots leave 5)
    h.send({ type: 'tx_fifo', dev: 0, slots: 16 });
    sendFrames(211, 230);
    await sleep(200);
    f = wire();
    check(f.length === 16, 'tx_fifo: with 16 slots only 5 more frames go out (' + f.length + ' on the wire in all, 16 expected)');
    h.send({ type: 'close', dev: 0 });
    await sleep(50);
}

async function testPcan() {
    const mock = createMockDevice(0x0C72, 0x0012, 'PCAN1');
    const h = createHarness(BUILD, SRC, mock, PACKED);
    const rec = (type, ts, body, total = 16) => { const r = [total & 255, total >> 8, type & 255, type >> 8]; le32(r, ts); le32(r, 0); r.push(...body); while (r.length < total) r.push(0); return r; };
    h.send({ type: 'init', timeOrigin: performance.timeOrigin });
    h.send({ type: 'open', dev: 0, epoch: 1, vid: 0x0C72, pid: 0x0012, serial: 'PCAN1' });
    const opened = await h.waitFor(m => m.type === 'opened', 3000, 'opened', C);
    check(opened && opened.protocol === 1, 'pcan: opened as PEAK');
    h.send({ type: 'rx', dev: 0, enable: true, ep: 0, size: 0 });
    await sleep(30);
    let ts = 5_000_000;
    for (let i = 0; i < 6; i++) {
        const pkt = [...rec(0x0100, ts, [i, 0]), ...rec(0x0004, ts, [0, 0, 0x00, 0x10]), ...(i === 2 ? rec(0x0002, ts, [0x90, 0, 12, 34]) : []),
                     ...(i === 3 ? rec(0x0003, ts, [0x80]) : []), 0, 0, 0, 0];
        mock.pushRx(pkt); ts += 1_000_000; await sleep(40);
    }
    await sleep(100);
    const ev = h.posted.filter(m => m.type === 'events').map(m => m.text).join('');
    check(/U100\n/.test(ev) && /E00000080:0000820000000C22\n/.test(ev) && /S3\n/.test(ev), 'pcan: events ' + JSON.stringify(ev));
    const syncs = h.posted.filter(m => m.type === 'sync');
    check(syncs.length >= 5 && syncs[syncs.length - 1].s[0] === 6 && syncs[syncs.length - 1].s[8] === 1, 'pcan: sync messages from calibration records (' + syncs.length + ')');
    check(!mock.ctrlLog.some(c => c.request === 6), 'pcan: no timestamp request for PEAK');
    check(!!h.posted.filter(m => m.type === 'snapshot').pop(), 'pcan: snapshot with the error frames');
    h.send({ type: 'rx', dev: 0, enable: false });
    h.send({ type: 'close', dev: 0 });
    await sleep(50);
}

(async () => {
    await testGsUsbElmue();
    await testElmueTxBlobs();
    await testPcan();
    console.log(C.n + ' checks, ' + C.fails + ' failed');
    process.exit(C.fails ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
