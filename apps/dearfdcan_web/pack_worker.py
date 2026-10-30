#!/usr/bin/env python3
"""Packs the workers into JavaScript strings for a single-file page.

Usage: pack_worker.py <can_core.js (built with -sSINGLE_FILE=1)> <out.js>
                      --usb-worker <usb_worker.js> [--usb-lib <library.js> ...]
                      --serial-worker <serial_worker.js> [--serial-lib <library.js> ...]

The output is a --pre-js file for the page build that defines
  Module['usbWorkerSource']     the core's Emscripten glue (wasm embedded in the glue, so the worker needs no
                                network access), the --usb-lib files (e.g. webusb_io.js) in the order given, and
                                usb_worker.js wrapped in a function to keep it strict and out of the glue's scope
  Module['serialWorkerSource']  the --serial-lib files (webserial_io.js) and serial_worker.js wrapped the same way
The libraries define one global each (WebUsbIo, WebSerialIo); the workers load them with importScripts() only when
that global is missing, which is what makes the same worker file run packed and as separate files.
The page starts the workers from these strings through Blob URLs (see js_webusb_init in webusb_bridge.cpp).
"""
import argparse
import json
import os
import sys


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


def wrapped(worker_source):
    return ';(function () {\n' + worker_source + '\n})();\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('core_js')
    ap.add_argument('out_js')
    ap.add_argument('--usb-worker', required=True)
    ap.add_argument('--usb-lib', action='append', default=[])
    ap.add_argument('--serial-worker', required=True)
    ap.add_argument('--serial-lib', action='append', default=[])
    args = ap.parse_args()

    core = read(args.core_js)
    if 'createCanCore' not in core:
        sys.exit(f'{args.core_js}: createCanCore factory not found (build with -sMODULARIZE=1 -sEXPORT_NAME=createCanCore)')
    # A non-embedded build fetches "<name>.wasm" relative to the script, which cannot work from a Blob URL
    wasm_name = os.path.splitext(os.path.basename(args.core_js))[0] + '.wasm'
    if wasm_name in core:
        sys.exit(f'{args.core_js}: wasm is not embedded (it refers to {wasm_name}); build with -sSINGLE_FILE=1')

    usb = core + '\n' + ''.join(read(p) + '\n' for p in args.usb_lib) + wrapped(read(args.usb_worker))
    serial = ''.join(read(p) + '\n' for p in args.serial_lib) + wrapped(read(args.serial_worker))

    def literal(text):
        s = json.dumps(text)  # ASCII-only, valid JavaScript string literal
        # The page JS is inlined into index.html: keep the HTML parser from seeing tags/comments in it
        return s.replace('</', '<\\/').replace('<!--', '<\\!--')

    with open(args.out_js, 'w', encoding='utf-8') as f:
        f.write(f"Module['usbWorkerSource'] = {literal(usb)};\n")
        f.write(f"Module['serialWorkerSource'] = {literal(serial)};\n")
    print(f'pack_worker: {args.out_js} ({len(usb)} bytes of CAN worker source, {len(serial)} bytes of serial worker source)')


if __name__ == '__main__':
    main()
