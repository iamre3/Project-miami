// sc_opt_common.h — shared helpers for the optimization subsystem
// (docs/OPTIMIZATION.md). FNV-1a 64-bit hashing and QPC wall timing.
// Header-only so every opt_*.cpp and the guest can use them without
// another link dependency. No game code lives here.
#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>

namespace sc {

// FNV-1a 64-bit. Seed defaults to the FNV offset basis.
inline uint64_t ScOptFnv1a(const void* data, size_t len,
                           uint64_t seed = 14695981039346656037ULL) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = seed;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

inline uint64_t ScOptFnv1aStr(const char* s, uint64_t seed) {
    return ScOptFnv1a(s, strlen(s), seed);
}

// Append helpers for building a canonical hash incrementally.
inline uint64_t ScOptHashBytes(uint64_t h, const void* data, size_t len) {
    return ScOptFnv1a(data, len, h);
}
inline uint64_t ScOptHashU32(uint64_t h, uint32_t v) {
    return ScOptFnv1a(&v, sizeof(v), h);
}
inline uint64_t ScOptHashU64(uint64_t h, uint64_t v) {
    return ScOptFnv1a(&v, sizeof(v), h);
}
inline uint64_t ScOptHashPtr(uint64_t h, const void* p) {
    return ScOptFnv1a(&p, sizeof(p), h);
}

// QPC wall timer. Start() stamps; ElapsedMs() returns ms since Start.
struct ScOptTimer {
    LARGE_INTEGER start_ = {};
    LARGE_INTEGER freq_ = {};
    void Start() {
        QueryPerformanceFrequency(&freq_);
        QueryPerformanceCounter(&start_);
    }
    double ElapsedMs() const {
        LARGE_INTEGER now = {};
        QueryPerformanceCounter(&now);
        return (double)(now.QuadPart - start_.QuadPart) * 1000.0 /
               (double)freq_.QuadPart;
    }
};

}  // namespace sc
