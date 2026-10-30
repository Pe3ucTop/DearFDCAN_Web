# browser-clipboard: the browser's clipboard for a Dear ImGui page (JavaScript, no build step). The monitor links it as a --pre-js and
# reaches it through apps/dearfdcan_web/clipboard.cpp. The test with a mock browser is tests/test_browser_clipboard.js.
set(BROWSER_CLIPBOARD_JS ${CMAKE_CURRENT_SOURCE_DIR}/browser_clipboard.js CACHE INTERNAL "Browser clipboard library")
