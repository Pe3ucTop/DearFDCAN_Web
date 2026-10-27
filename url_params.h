#pragma once
// Parsing of the URL query parameters that preset the bit-timing UI, e.g.
//   index.html?baudrate=500k&sample_point=87.5&data_baudrate=2M&data_sample_point=75
// Pure functions without browser dependencies (the query string itself is read in app_ui.cpp).
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <strings.h>

constexpr int kMinBitrateKbps = 10;
constexpr int kMaxBitrateKbps = 12000;
constexpr float kMinSamplePointPct = 50.0f;  // same range as the UI slider
constexpr float kMaxSamplePointPct = 95.0f;

// Bit rate in kbit/s. Accepted forms:
//   500k, 500K, 500kbps   kbit/s          2M, 2m, 2Mbps   Mbit/s
//   500                   plain number: below 10000 it is kbit/s, otherwise bit/s (500000 = 500 kbit/s)
// The UI works with whole kbit/s, so finer values are rounded. Values outside 10 kbit/s .. 12 Mbit/s are
// rejected (a typo should not silently turn into a clamped rate).
inline bool ParseBitrateKbps(const char* text, int& out_kbps) {
    if (!text) return false;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || !std::isfinite(v) || v <= 0.0) return false;
    while (std::isspace(static_cast<unsigned char>(*end))) ++end;

    double kbps = 0.0;
    switch (*end) {
        case 'k': case 'K': kbps = v;          ++end; break;
        case 'm': case 'M': kbps = v * 1000.0; ++end; break;
        case '\0':          kbps = v < 10000.0 ? v : v / 1000.0; break;
        default: return false;
    }
    if (strncasecmp(end, "bps", 3) == 0) end += 3;
    while (std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0') return false;

    const double rounded = std::floor(kbps + 0.5);
    if (rounded < kMinBitrateKbps || rounded > kMaxBitrateKbps) return false;
    out_kbps = static_cast<int>(rounded);
    return true;
}

// Sample point in percent: 87.5, 87.5% or as a fraction (0.875). Must lie within 50 .. 95 %.
inline bool ParseSamplePointPct(const char* text, float& out_pct) {
    if (!text) return false;
    char* end = nullptr;
    double v = std::strtod(text, &end);
    if (end == text || !std::isfinite(v) || v <= 0.0) return false;
    while (std::isspace(static_cast<unsigned char>(*end))) ++end;
    const bool percent_sign = (*end == '%');
    if (percent_sign) ++end;
    while (std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0') return false;

    if (!percent_sign && v <= 1.0) v *= 100.0;   // 0.875 -> 87.5 %
    if (v < kMinSamplePointPct || v > kMaxSamplePointPct) return false;
    out_pct = static_cast<float>(v);
    return true;
}
