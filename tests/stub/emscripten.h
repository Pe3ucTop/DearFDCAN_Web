#pragma once
// Minimal stand-in for <emscripten.h> so that app_ui.cpp compiles natively in tests/test_app_ui.cpp:
// EM_JS functions become empty functions that return a default value.
#define EM_JS(ret, name, params, ...) ret name params { return ret(); }
#define EMSCRIPTEN_KEEPALIVE
double emscripten_get_now();
