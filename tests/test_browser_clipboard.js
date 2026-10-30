'use strict';
// libs/browser-clipboard against a mock browser: paste events, writeText that succeeds / is refused / throws / does not exist,
// the execCommand fallback, a text that waits for the next user action, the newest write winning.
const BrowserClipboard = require('../libs/browser-clipboard/browser_clipboard.js');

let checks = 0, fails = 0;
function check(cond, text) { checks++; if (!cond) { fails++; console.log('FAIL: ' + text); } }
const tick = () => new Promise(r => setTimeout(r, 0));

function mockEnv(opts = {}) {
    const listeners = new Map();                       // 'target:type' -> [fn]
    const target = name => ({
        addEventListener(type, fn) { const k = name + ':' + type; if (!listeners.has(k)) listeners.set(k, []); listeners.get(k).push(fn); },
        removeEventListener(type, fn) { const k = name + ':' + type; const a = listeners.get(k) || []; const i = a.indexOf(fn); if (i >= 0) a.splice(i, 1); },
    });
    const env = { log: [], clock: 1000, focused: null, execResult: opts.execResult !== false, writes: [], execs: [] };
    const active = { focus() { env.focused = 'canvas'; } };
    const body = { children: [], appendChild(c) { this.children.push(c); env.log.push('append'); }, removeChild(c) { this.children.splice(this.children.indexOf(c), 1); env.log.push('remove'); } };
    env.document = Object.assign(target('doc'), {
        body, activeElement: active,
        createElement(tag) { return { tag, value: '', style: {}, attrs: {}, setAttribute(k, v) { this.attrs[k] = v; }, select() { env.log.push('select'); } }; },
        execCommand(cmd) { env.execs.push({ cmd, value: body.children.length ? body.children[0].value : null }); return env.execResult; },
    });
    env.window = target('win');
    env.write = opts.write || (text => { env.writes.push(text); return Promise.resolve(); });
    env.navigator = opts.noClipboard ? {} : { clipboard: { writeText: text => env.write(text) } };
    env.now = () => env.clock;
    env.count = (name, type) => (listeners.get(name + ':' + type) || []).length;
    env.emit = (name, type, ev) => { for (const fn of (listeners.get(name + ':' + type) || []).slice()) fn(ev || {}); };
    return env;
}

async function testWrite() {
    const env = mockEnv();
    const clip = BrowserClipboard.create(env);
    clip.install();
    check(clip.status().state === 'idle' && clip.get() === '', 'a new clipboard is idle and empty');
    env.clock = 2000;
    clip.set('Привет, CAN FD');
    check(env.writes.length === 1 && env.writes[0] === 'Привет, CAN FD' && clip.get() === 'Привет, CAN FD', 'set() writes at once and remembers the text (UTF-8)');
    await tick();
    const s = clip.status();
    check(s.state === 'copied' && s.at === 2000 && !s.pending, 'writeText accepted: copied, with the time (' + JSON.stringify(s) + ')');
}

async function testPaste() {
    const env = mockEnv();
    const clip = BrowserClipboard.create(env);
    clip.install();
    env.emit('doc', 'paste', { clipboardData: { getData: type => type === 'text/plain' ? 'from outside' : '' } });
    check(clip.get() === 'from outside', 'a paste event gives the text to get()');
    env.emit('doc', 'paste', {});
    env.emit('doc', 'paste', { clipboardData: { getData: () => undefined } });
    check(clip.get() === 'from outside', 'a paste event without text changes nothing');
    env.emit('doc', 'paste', { clipboardData: { getData: () => '' } });
    check(clip.get() === '', 'an empty clipboard is an empty paste');
    clip.set('mine');
    env.emit('doc', 'paste', { clipboardData: { getData: () => 'newer' } });
    check(clip.get() === 'newer', 'the later of copy and paste wins');
}

async function testFallbacks() {
    // writeText refused, execCommand works: the hidden element is selected, copied, removed; the focus returns
    let env = mockEnv({ write: () => Promise.reject(new Error('NotAllowedError')) });
    let clip = BrowserClipboard.create(env);
    clip.install();
    clip.set('abc');
    await tick();
    check(env.execs.length === 1 && env.execs[0].cmd === 'copy' && env.execs[0].value === 'abc', 'refused writeText: execCommand copies the text');
    check(env.log.join() === 'append,select,remove' && env.focused === 'canvas', 'the hidden element is removed and the focus given back');
    check(clip.status().state === 'copied' && !clip.status().pending, 'the fallback counts as copied');

    // writeText throws, or does not exist (no secure context)
    env = mockEnv({ write: () => { throw new Error('SecurityError'); } });
    clip = BrowserClipboard.create(env); clip.install(); clip.set('x');
    check(env.execs.length === 1 && clip.status().state === 'copied', 'writeText that throws: fallback');
    env = mockEnv({ noClipboard: true });
    clip = BrowserClipboard.create(env); clip.install(); clip.set('y');
    check(env.execs.length === 1 && env.execs[0].value === 'y' && clip.status().state === 'copied', 'no navigator.clipboard: fallback');

    // both refused: the text waits
    env = mockEnv({ write: () => Promise.reject(new Error('NotAllowedError')), execResult: false });
    clip = BrowserClipboard.create(env); clip.install();
    env.clock = 5000;
    clip.set('waiting');
    await tick();
    let s = clip.status();
    check(s.state === 'blocked' && s.pending && s.at === 5000 && clip.get() === 'waiting', 'both refused: blocked, the text waits and is still what get() returns');

    // a copy event takes the waiting text
    let data = null, prevented = false;
    env.clock = 6000;
    env.emit('doc', 'copy', { clipboardData: { setData: (t, v) => { data = [t, v]; } }, preventDefault() { prevented = true; } });
    s = clip.status();
    check(data && data[0] === 'text/plain' && data[1] === 'waiting' && prevented && s.state === 'copied' && !s.pending && s.at === 6000, 'a copy event takes the waiting text');
    data = null;
    env.emit('doc', 'copy', { clipboardData: { setData: (t, v) => { data = [t, v]; } }, preventDefault() { prevented = true; } });
    check(data === null, 'a copy event with nothing waiting is left alone');

    // the next user action writes it
    env = mockEnv({ write: () => Promise.reject(new Error('NotAllowedError')), execResult: false });
    clip = BrowserClipboard.create(env); clip.install();
    clip.set('later');
    await tick();
    check(clip.status().state === 'blocked', 'blocked again');
    env.write = text => { env.writes.push(text); return Promise.resolve(); };      // the browser allows it now
    env.emit('win', 'pointerup', {});
    await tick();
    check(env.writes.length === 1 && env.writes[0] === 'later' && clip.status().state === 'copied' && !clip.status().pending, 'the next click writes the waiting text');
    env.emit('win', 'keydown', {});
    await tick();
    check(env.writes.length === 1, 'nothing is written again once it is done');

    // a still refused gesture keeps the text waiting
    env = mockEnv({ write: () => Promise.reject(new Error('NotAllowedError')), execResult: false });
    clip = BrowserClipboard.create(env); clip.install(); clip.set('t');
    await tick();
    env.emit('win', 'keyup', {});
    await tick();
    check(clip.status().state === 'blocked' && clip.status().pending, 'a refused retry keeps the text waiting');
}

async function testNewestWins() {
    const pend = [];
    const env = mockEnv({ write: text => new Promise((res, rej) => pend.push({ text, res, rej })), execResult: false });
    const clip = BrowserClipboard.create(env);
    clip.install();
    clip.set('A');
    clip.set('B');
    pend[1].res();
    await tick();
    check(clip.status().state === 'copied', 'the newest write finished');
    pend[0].rej(new Error('late'));
    await tick();
    check(clip.status().state === 'copied' && !clip.status().pending && clip.get() === 'B', 'a late failure of an older write changes nothing');
}

async function testInstall() {
    const env = mockEnv();
    const clip = BrowserClipboard.create(env);
    clip.install(); clip.install();
    check(env.count('doc', 'paste') === 1 && env.count('doc', 'copy') === 1 && env.count('win', 'pointerup') === 1, 'install() twice adds the listeners once');
    clip.dispose();
    check(env.count('doc', 'paste') === 0 && env.count('doc', 'copy') === 0 && env.count('win', 'pointerup') === 0 && env.count('win', 'keydown') === 0, 'dispose() removes them');
    clip.dispose();
}

(async () => {
    await testWrite();
    await testPaste();
    await testFallbacks();
    await testNewestWins();
    await testInstall();
    console.log(checks + ' checks, ' + fails + ' failed');
    process.exit(fails ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
