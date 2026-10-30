// browser-clipboard: the clipboard of the browser for a program that draws its own user interface (Dear ImGui on a canvas) and
// asks for the clipboard synchronously.
//
// Reading: the browser gives the pasted text only to a `paste` event, so the text of the last paste is kept and handed out by get().
// Writing: set() writes at once with navigator.clipboard.writeText. Browsers allow that only for a short time after a user action
// (and only in a secure context); if it is refused, the older document.execCommand('copy') is tried, and if that is refused too the text
// waits ("blocked") and is written by the next click or key press, or handed to the next `copy` event. status() tells which of these
// happened, so the program can show "Copied" or ask the user to click again.
//
//   const clip = BrowserClipboard.create({window, document, navigator, now});   // now() in ms; the defaults are the browser's own
//   clip.install();                      // listeners on the document / window (once)
//   clip.set(text);   clip.get();   clip.status() -> {state: 'idle'|'copied'|'blocked', at: ms of the last change, pending: bool}
//   clip.dispose();
//
// The file defines the global BrowserClipboard (and Module['BrowserClipboard'] when it is a --pre-js of an Emscripten program); under
// Node it is a module (tests/test_browser_clipboard.js).
(function (root) {
    'use strict';

    const GESTURE_EVENTS = ['pointerup', 'keydown', 'keyup'];

    function create(env) {
        env = env || {};
        const win = env.window || (typeof window !== 'undefined' ? window : null);
        const doc = env.document || (typeof document !== 'undefined' ? document : null);
        const nav = env.navigator || (typeof navigator !== 'undefined' ? navigator : null);
        const now = env.now || (() => Date.now());

        const st = { text: '', pending: null, state: 'idle', at: 0, token: 0, installed: false };

        function setState(state) { st.state = state; st.at = now(); }
        function done() { st.pending = null; setState('copied'); }

        // The old way: select the text of a hidden element and let the browser copy it. The program's focus is given back afterwards.
        function execCopy(text) {
            if (!doc || !doc.createElement || !doc.execCommand || !doc.body) return false;
            const active = doc.activeElement;
            let ok = false;
            try {
                const area = doc.createElement('textarea');
                area.value = text;
                area.setAttribute('readonly', '');
                area.style.cssText = 'position:fixed;left:-1000px;top:0;opacity:0';
                doc.body.appendChild(area);
                area.select();
                try { ok = !!doc.execCommand('copy'); } finally { doc.body.removeChild(area); }
            } catch (e) {
                ok = false;
            }
            try { if (active && active.focus) active.focus(); } catch (e) { /* nothing to restore */ }
            return ok;
        }

        // Only the newest write may change the state: a late answer for an older text is ignored
        function write(text) {
            const token = ++st.token;
            const refused = () => {
                if (token !== st.token) return;
                if (execCopy(text)) done();
                else { st.pending = text; setState('blocked'); }
            };
            if (!nav || !nav.clipboard || !nav.clipboard.writeText) { refused(); return; }
            let p;
            try { p = nav.clipboard.writeText(text); } catch (e) { refused(); return; }
            Promise.resolve(p).then(() => { if (token === st.token) done(); }, refused);
        }

        function onPaste(e) {
            const data = e && e.clipboardData;
            if (!data || typeof data.getData !== 'function') return;
            const text = data.getData('text/plain');
            if (typeof text === 'string') st.text = text;
        }

        // A `copy` event (Ctrl+C) while a text is waiting: the browser takes it from us
        function onCopy(e) {
            if (st.pending === null || !e || !e.clipboardData) return;
            e.clipboardData.setData('text/plain', st.pending);
            if (e.preventDefault) e.preventDefault();
            done();
        }

        function onGesture() {
            if (st.pending !== null) write(st.pending);
        }

        return {
            install() {
                if (st.installed) return;
                st.installed = true;
                if (doc) { doc.addEventListener('paste', onPaste); doc.addEventListener('copy', onCopy); }
                if (win) for (const type of GESTURE_EVENTS) win.addEventListener(type, onGesture, true);
            },
            dispose() {
                if (!st.installed) return;
                st.installed = false;
                if (doc) { doc.removeEventListener('paste', onPaste); doc.removeEventListener('copy', onCopy); }
                if (win) for (const type of GESTURE_EVENTS) win.removeEventListener(type, onGesture, true);
            },
            set(text) {
                st.text = String(text);
                st.pending = null;
                write(st.text);
            },
            get() { return st.text; },
            status() { return { state: st.state, at: st.at, pending: st.pending !== null }; },
        };
    }

    const api = { create: create };
    if (typeof module === 'object' && module && module.exports) module.exports = api;
    else if (root) root.BrowserClipboard = api;
    if (typeof Module === 'object' && Module) Module['BrowserClipboard'] = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
