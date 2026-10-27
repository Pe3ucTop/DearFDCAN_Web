#!/usr/bin/env python3
"""Packs the USB worker into a single JavaScript string for a single-file page.

Usage: pack_worker.py <can_core.js (built with -sSINGLE_FILE=1)> <usb_worker.js> <out.js>

The output is a --pre-js file for the page build that defines Module['usbWorkerSource']: the core's
Emscripten glue (wasm embedded in the glue, so the worker needs no network access) followed by
usb_worker.js wrapped in a function to keep it strict and out of the glue's scope. The page starts the
worker from this string through a Blob URL (see js_webusb_init in webusb_bridge.cpp).
"""
import json
import os
import sys


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    core_path, worker_path, out_path = sys.argv[1:]

    with open(core_path, encoding='utf-8') as f:
        core = f.read()
    with open(worker_path, encoding='utf-8') as f:
        worker = f.read()

    if 'createCanCore' not in core:
        sys.exit(f'{core_path}: createCanCore factory not found (build with -sMODULARIZE=1 -sEXPORT_NAME=createCanCore)')
    # A non-embedded build fetches "<name>.wasm" relative to the script, which cannot work from a Blob URL
    wasm_name = os.path.splitext(os.path.basename(core_path))[0] + '.wasm'
    if wasm_name in core:
        sys.exit(f'{core_path}: wasm is not embedded (it refers to {wasm_name}); build with -sSINGLE_FILE=1')

    bundle = core + '\n;(function () {\n' + worker + '\n})();\n'

    literal = json.dumps(bundle)  # ASCII-only, valid JavaScript string literal
    # The page JS is inlined into index.html: keep the HTML parser from seeing tags/comments in it
    literal = literal.replace('</', '<\\/').replace('<!--', '<\\!--')

    with open(out_path, 'w', encoding='utf-8') as f:
        f.write(f"Module['usbWorkerSource'] = {literal};\n")
    print(f'pack_worker: {out_path} ({len(bundle)} bytes of worker source)')


if __name__ == '__main__':
    main()
